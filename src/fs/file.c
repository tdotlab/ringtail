// Per-task file descriptors and current directory. A program starts
// with console input on fd 0 and console output on 1 and 2, or with
// copies of its parent's descriptors; the rest are diskfs files, pipe
// ends, sockets, channels and ptys. Its controlling terminal is its
// parent's, or its standard input's when that is a terminal. It starts in its parent's directory. All
// buffers passed in are user pointers; data moves through a kernel
// bounce buffer, so the disk code never touches user memory.
//
// A descriptor points at an open file (ofile_t, fs.h), and copies of
// it, from dup2 or handed to a child by spawn, point at the same one:
// they share its offset, so a child's output to a redirected file
// lands after its parent's and the parent's next write lands after
// the child's, as in POSIX. Rules that keep this safe:
//  - Only a task changes its own descriptor table (task_exit closes
//    the dying task's own), so an open file a task is blocked on in
//    read or accept cannot go away under it: that task's own
//    reference keeps it.
//  - A descriptor is taken out of the table before its reference is
//    dropped, since dropping the last one can block (the filesystem
//    lock) and nothing may find it half freed.
//  - A file's offset moves under its mutex, so two tasks reading or
//    writing the same open file at once do not land on the same
//    bytes. Pipes and sockets have no offset and do not take it; they
//    can block for as long as the other end likes.
#include "fs.h"
#include "task.h"
#include "tty.h"
#include "console.h"
#include "lib.h"
#include "mm.h"
#include "ringtail/abi.h"
#include "net.h"
#include "arch.h"
#include "drivers.h"
#include "kernel.h"  // panic

#define BOUNCE 4096u

// The console's open files, shared by every task and never freed. A
// terminal file's tty is 0 for the console.
static ofile_t console_in = { .kind = FD_TTY, .flags = O_RDONLY };
static ofile_t console_out = { .kind = FD_TTY, .flags = O_WRONLY };

static struct tty* ftty(const ofile_t* f) { return f->tty ? f->tty : console_tty; }

static int ofile_static(const ofile_t* f) { return f == &console_in || f == &console_out; }

static ofile_t* ofile_new(fd_kind_t kind, int flags) {
    ofile_t* f = kzalloc(sizeof *f);
    if (!f) return 0;
    f->refs = 1;
    f->kind = kind;
    f->flags = flags;
    return f;
}

static ofile_t* ofile_get(ofile_t* f) {
    if (!ofile_static(f)) __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    return f;
}

// Drops a reference, and with the last one lets go of what the open
// file holds. That can block, so f is already out of every table.
static void ofile_put(ofile_t* f) {
    if (ofile_static(f)) return;
    int left = __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    if (left > 0) return;
    if (left < 0) panic("open file %p released once too often", (void*)f);
    if (f->kind == FD_FILE) diskfs_unref(f->node);
    else if (f->kind == FD_PIPE_R || f->kind == FD_PIPE_W) pipe_unref(f->pipe, f->kind == FD_PIPE_W);
    else if (f->kind == FD_SOCKET) udp_socket_unref(f->sock);
    else if (f->kind == FD_TCP) tcpsock_unref(f->tcp);
    else if (f->kind == FD_CHAN) chan_close(f->chan, f->side);
    else if (f->kind == FD_CHAN_LISTEN) chan_unlisten(f->listener);
    else if (f->kind == FD_TTY) tty_slave_unref(ftty(f));
    else if (f->kind == FD_PTY) pty_master_close(f->tty);
    f->kind = FD_NONE;
    kfree(f);
}

void files_init_std(task_t* t) {
    t->cwd = current ? current->cwd : DISKFS_ROOT;
    diskfs_ref(t->cwd);
    memset(t->fds, 0, sizeof t->fds);
    t->fds[STDIN_FILENO] = &console_in;
    t->fds[STDOUT_FILENO] = &console_out;
    t->fds[STDERR_FILENO] = &console_out;
    t->tty = current ? current->tty : 0;
    tty_ref(t->tty);
}

int file_fd_ok(int fd) {
    return fd >= 0 && fd < MAX_FDS && current->fds[fd];
}

void files_inherit(task_t* child, task_t* parent, const int std[SPAWN_FDS]) {
    for (int i = 0; i < SPAWN_FDS; i++) {
        if (std[i] < 0) continue;  // (only 3 can be: nothing there)
        ofile_t* f = parent->fds[std[i]];
        if (!f) continue;  // the caller checked; keep the console
        child->fds[i] = ofile_get(f);  // replaces a console one, which is not counted
    }
    ofile_t* in = child->fds[STDIN_FILENO];
    if (in->kind == FD_TTY && in->tty != child->tty) {
        tty_ref(in->tty);
        tty_unref(child->tty);
        child->tty = in->tty;
    }
}

void files_close_all(task_t* t) {
    for (int i = 0; i < MAX_FDS; i++) {
        ofile_t* f = t->fds[i];
        t->fds[i] = 0;
        if (f) ofile_put(f);
    }
    diskfs_unref(t->cwd);
    t->cwd = DISKFS_ROOT;
    struct tty* tty = t->tty;
    t->tty = 0;
    tty_unref(tty);
}

int file_cwd(void) { return current->cwd; }

int file_chdir(const char* path) {
    int node = diskfs_opendir(current->cwd, path);
    if (node < 0) return node;
    diskfs_unref(current->cwd);
    current->cwd = node;
    return 0;
}

static int free_fd(int from) {
    for (int i = from; i < MAX_FDS; i++)
        if (!current->fds[i]) return i;
    return -EMFILE;
}

static ofile_t* get_fd(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    return current->fds[fd];
}

int file_open(const char* uname, int flags) {
    char name[PATH_MAX];
    int n = strncpy_from_user(name, uname, sizeof name);
    if (n < 0) return n;
    int acc = flags & O_ACCMODE;
    if (acc == O_ACCMODE) return -EINVAL;
    if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND)) return -EINVAL;
    if ((flags & O_TRUNC) && acc == O_RDONLY) return -EINVAL;

    int fd = free_fd(0);
    if (fd < 0) return fd;

    // The terminal by name: it reaches the keyboard when standard input
    // is a pipe. One direction per descriptor, as the standard ones are.
    if (strcmp(name, DEV_TTY) == 0) {
        if (acc == O_RDWR || (flags & ~O_ACCMODE)) return -EINVAL;
        struct tty* t = tty_current();
        if (tty_is_console(t)) {
            current->fds[fd] = acc == O_RDONLY ? &console_in : &console_out;
            return fd;
        }
        ofile_t* f = ofile_new(FD_TTY, flags);
        if (!f) return -ENOMEM;
        f->tty = t;
        tty_slave_ref(t);
        current->fds[fd] = f;
        return fd;
    }

    // The node comes referenced, so nobody can delete it under us.
    int node = diskfs_lookup(current->cwd, name);
    if (node >= 0 && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        diskfs_unref(node);
        return -EEXIST;
    }
    if (node == -ENOENT && (flags & O_CREAT)) {
        node = diskfs_create(current->cwd, name);
        if (node == -EEXIST && !(flags & O_EXCL)) node = diskfs_lookup(current->cwd, name);  // lost a race
    }
    if (node < 0) return node;
    ofile_t* f = ofile_new(FD_FILE, flags);
    if (!f) { diskfs_unref(node); return -ENOMEM; }
    if (flags & O_TRUNC) {
        int r = diskfs_truncate(node, 0);
        if (r < 0) { kfree(f); diskfs_unref(node); return r; }
    }
    f->node = node;
    current->fds[fd] = f;
    return fd;
}

int file_close(int fd) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    current->fds[fd] = 0;
    ofile_put(f);
    return 0;
}

int file_pipe(int* ufds) {
    int fds[2];
    fds[0] = free_fd(0);
    if (fds[0] < 0) return fds[0];
    fds[1] = free_fd(fds[0] + 1);
    if (fds[1] < 0) return fds[1];
    if (user_range_ok((uintptr_t)ufds, sizeof fds, 1)) return -EFAULT;
    struct pipe* p = pipe_new();
    if (!p) return -ENOMEM;
    ofile_t* r = ofile_new(FD_PIPE_R, O_RDONLY);
    ofile_t* w = ofile_new(FD_PIPE_W, O_WRONLY);
    int err = !r || !w ? -ENOMEM : copy_to_user(ufds, fds, sizeof fds) ? -EFAULT : 0;
    if (err) {
        kfree(r);
        kfree(w);
        pipe_unref(p, 0);
        pipe_unref(p, 1);
        return err;
    }
    r->pipe = w->pipe = p;
    current->fds[fds[0]] = r;
    current->fds[fds[1]] = w;
    return 0;
}

struct tty* file_tty_of(int fd) {
    ofile_t* f = get_fd(fd);
    return f && f->kind == FD_TTY ? ftty(f) : 0;
}

int file_tty_size(int fd, int* cols, int* rows) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_TTY && f->kind != FD_PTY) return -ENOTTY;
    return tty_size(ftty(f), cols, rows);
}

int file_pty(int* ufds, const tty_size_t* usize) {
    tty_size_t z;
    if (copy_from_user(&z, usize, sizeof z)) return -EFAULT;
    int fds[2];
    fds[0] = free_fd(0);
    if (fds[0] < 0) return fds[0];
    fds[1] = free_fd(fds[0] + 1);
    if (fds[1] < 0) return fds[1];
    if (user_range_ok((uintptr_t)ufds, sizeof fds, 1)) return -EFAULT;
    if (z.cols < 1 || z.rows < 1 || z.cols > 1000 || z.rows > 1000) return -EINVAL;
    struct tty* t = pty_new(z.cols, z.rows);
    if (!t) return -ENOMEM;
    ofile_t* m = ofile_new(FD_PTY, O_RDWR);
    ofile_t* s = ofile_new(FD_TTY, O_RDWR);
    int err = !m || !s ? -ENOMEM : copy_to_user(ufds, fds, sizeof fds) ? -EFAULT : 0;
    if (err) {
        kfree(m);
        kfree(s);
        pty_master_close(t);
        return err;
    }
    m->tty = s->tty = t;
    tty_slave_ref(t);
    current->fds[fds[0]] = m;
    current->fds[fds[1]] = s;
    return 0;
}

int file_socket(int type) {
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -EINVAL;
    int fd = free_fd(0);
    if (fd < 0) return fd;
    ofile_t* f = ofile_new(type == SOCK_DGRAM ? FD_SOCKET : FD_TCP, O_RDWR);
    if (!f) return -ENOMEM;
    if (type == SOCK_DGRAM) f->sock = udp_socket_new();
    else f->tcp = tcpsock_new();
    if (!f->sock && !f->tcp) {
        kfree(f);
        return -ENFILE;
    }
    current->fds[fd] = f;
    return fd;
}

int file_get_socket(int fd, struct socket** s) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind == FD_TCP) return -EOPNOTSUPP;
    if (f->kind != FD_SOCKET) return -ENOTSOCK;
    *s = f->sock;
    return 0;
}

int file_get_tcp(int fd, struct tcp** c) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind == FD_SOCKET) return -EOPNOTSUPP;
    if (f->kind != FD_TCP) return -ENOTSOCK;
    *c = f->tcp;
    return 0;
}

// A channel name from the caller.
static int chan_name(const char* uname, char name[CHAN_NAME_MAX + 1]) {
    int n = strncpy_from_user(name, uname, CHAN_NAME_MAX + 1);
    if (n == -ENAMETOOLONG) return -EINVAL;
    if (n < 0) return n;
    return chan_name_ok(name) ? 0 : -EINVAL;
}

int file_chan_listen(const char* uname) {
    char name[CHAN_NAME_MAX + 1];
    int r = chan_name(uname, name);
    if (r) return r;
    int fd = free_fd(0);
    if (fd < 0) return fd;
    ofile_t* f = ofile_new(FD_CHAN_LISTEN, O_RDONLY);
    if (!f) return -ENOMEM;
    r = chan_listen(name, &f->listener);
    if (r) { kfree(f); return r; }
    current->fds[fd] = f;
    return fd;
}

int file_chan_connect(const char* uname) {
    char name[CHAN_NAME_MAX + 1];
    int r = chan_name(uname, name);
    if (r) return r;
    int fd = free_fd(0);
    if (fd < 0) return fd;
    ofile_t* f = ofile_new(FD_CHAN, O_RDWR);
    if (!f) return -ENOMEM;
    r = chan_connect(name, &f->chan);
    if (r) { kfree(f); return r; }
    f->side = 0;
    current->fds[fd] = f;
    return fd;
}

// A channel listener's next connection; uaddr is not used.
static int chan_accept_fd(ofile_t* lf, int timeout_ms) {
    int nfd = free_fd(0);
    if (nfd < 0) return nfd;
    ofile_t* f = ofile_new(FD_CHAN, O_RDWR);
    if (!f) return -ENOMEM;
    int r = chan_accept(lf->listener, &f->chan, timeout_ms);
    if (r) { kfree(f); return r; }
    f->side = 1;
    // nfd is still free: a task is one thread, and it was waiting.
    current->fds[nfd] = f;
    return nfd;
}

int file_accept(int fd, sock_addr_t* uaddr, int timeout_ms) {
    ofile_t* lf = get_fd(fd);
    if (lf && lf->kind == FD_CHAN_LISTEN) return chan_accept_fd(lf, timeout_ms);
    struct tcp* l;
    int r = file_get_tcp(fd, &l);
    if (r) return r;
    // Everything that could fail is checked before a connection is
    // taken, so none is lost.
    int nfd = free_fd(0);
    if (nfd < 0) return nfd;
    if (uaddr && user_range_ok((uintptr_t)uaddr, sizeof *uaddr, 1)) return -EFAULT;
    ofile_t* f = ofile_new(FD_TCP, O_RDWR);
    if (!f) return -ENOMEM;
    struct tcp* c;
    sock_addr_t a;
    uint32_t ip;
    r = tcpsock_accept(l, &c, &ip, &a.port, timeout_ms);
    if (r) { kfree(f); return r; }
    a.ip = ip;
    f->tcp = c;
    current->fds[nfd] = f;
    if (uaddr && copy_to_user(uaddr, &a, sizeof a)) return -EFAULT;  // the fd is open all the same
    return nfd;
}

int file_fdtype(int fd) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    switch (f->kind) {
    case FD_TTY: return FDTYPE_TTY;
    case FD_PTY: return FDTYPE_PTY;
    case FD_FILE: return FDTYPE_FILE;
    case FD_PIPE_R:
    case FD_PIPE_W: return FDTYPE_PIPE;
    case FD_SOCKET: return FDTYPE_UDP;
    case FD_TCP: return FDTYPE_TCP;
    case FD_CHAN:
    case FD_CHAN_LISTEN: return FDTYPE_CHAN;
    default: return -EBADF;
    }
}

// ---- Polling ----

static volatile uint32_t poll_gen;  // counts the changes a poller may care about

void poll_notify(void) {
    uint32_t f = irq_save();
    poll_gen++;
    irq_restore(f);
    task_wakeup((void*)&poll_gen);
}

static int fd_events(int fd) {
    ofile_t* f = get_fd(fd);
    if (!f) return POLLNVAL;
    switch (f->kind) {
    case FD_TTY: {
        struct tty* t = ftty(f);
        int acc = f->flags & O_ACCMODE, ev = 0;
        if (acc != O_WRONLY && tty_has_input(t)) ev |= POLLIN;
        if (acc != O_RDONLY) ev |= tty_is_console(t) ? POLLIN | POLLOUT : tty_out_events(t);
        return ev;
    }
    case FD_PTY: return pty_master_events(f->tty);
    case FD_PIPE_R: return pipe_events(f->pipe, 0);
    case FD_PIPE_W: return pipe_events(f->pipe, 1);
    case FD_SOCKET: return udp_socket_events(f->sock);
    case FD_TCP: return tcpsock_events(f->tcp);
    case FD_CHAN: return chan_events(f->chan, f->side);
    case FD_CHAN_LISTEN: return chan_listener_events(f->listener);
    default: return POLLIN | POLLOUT;  // files and console output never wait
    }
}

// A change is noted by poll_gen before anyone is woken, so reading it
// before looking and sleeping only if it has not moved loses nothing.
// The sleep is cut short every 100 ms anyway, for the one change
// nobody announces: the terminal passing to another job.
int file_pollfd(pollfd_t* ufds, uint32_t n, int timeout_ms) {
    if (n > MAX_FDS) return -EINVAL;
    pollfd_t p[MAX_FDS];
    uint32_t size = n * (uint32_t)sizeof *p;
    if (copy_from_user(p, ufds, size) || user_range_ok((uintptr_t)ufds, size, 1)) return -EFAULT;
    uint64_t ticks = timeout_ms > 0 ? ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000 : 0;
    uint32_t until = timer_ticks() + (uint32_t)ticks;
    int count;
    for (;;) {
        uint32_t gen = poll_gen;
        count = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (p[i].fd == POLLFD_INPUT) p[i].revents = gfx_input_events() & p[i].events;
            else p[i].revents = p[i].fd < 0 ? 0 : fd_events(p[i].fd) & (p[i].events | POLLERR | POLLHUP | POLLNVAL);
            count += p[i].revents != 0;
        }
        if (count || timeout_ms == 0) break;
        if (task_signalled(current)) return -EINTR;
        uint32_t now = timer_ticks(), wake = now + TIMER_HZ / 10;
        if (timeout_ms > 0) {
            if ((int32_t)(now - until) >= 0) break;
            if ((int32_t)(wake - until) > 0) wake = until;
        }
        uint32_t f = irq_save();
        if (poll_gen == gen && !task_signalled(current)) task_block_timeout((void*)&poll_gen, wake);
        irq_restore(f);
    }
    if (copy_to_user(ufds, p, size)) return -EFAULT;
    return count;
}

// newfd comes to point at oldfd's open file, offset and all.
int file_dup2(int oldfd, int newfd) {
    ofile_t* f = get_fd(oldfd);
    if (!f || newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    ofile_t* old = current->fds[newfd];
    current->fds[newfd] = ofile_get(f);  // before the put, in case both are the same
    if (old) ofile_put(old);
    return newfd;
}

static int terminal_read(struct tty* t, void* ubuf, uint32_t len) {
    if (current->job != tty_foreground(t)) return -EIO;  // background job
    char buf[256];
    if (len > sizeof buf) len = sizeof buf;
    if (user_range_ok((uintptr_t)ubuf, len, 1)) return -EFAULT;
    int n = tty_read(t, buf, len);
    if (n > 0 && copy_to_user(ubuf, buf, (size_t)n)) return -EFAULT;
    return n;
}

int file_read(int fd, void* ubuf, uint32_t len) {
    ofile_t* f = get_fd(fd);
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->kind == FD_SOCKET) return -EINVAL;  // UDP sockets take SYS_RECVFROM
    if (len == 0) return 0;
    if (f->kind == FD_TTY) return terminal_read(ftty(f), ubuf, len);
    if (f->kind == FD_CHAN) {
        if (user_range_ok((uintptr_t)ubuf, MIN(len, CHAN_MSG_MAX), 1)) return -EFAULT;
        uint8_t msg[CHAN_MSG_MAX];
        int n = chan_read(f->chan, f->side, msg, MIN(len, CHAN_MSG_MAX));
        if (n > 0 && copy_to_user(ubuf, msg, (size_t)n)) n = -EFAULT;  // the message is gone all the same
        return n;
    }
    if (f->kind != FD_FILE && f->kind != FD_PIPE_R && f->kind != FD_TCP && f->kind != FD_PTY) return -EBADF;
    if (user_range_ok((uintptr_t)ubuf, len, 1)) return -EFAULT;

    uint8_t* bounce = kmalloc(BOUNCE);
    if (!bounce) return -ENOMEM;
    if (f->kind == FD_PIPE_R || f->kind == FD_TCP || f->kind == FD_PTY) {
        // Whatever is there now, waiting only if nothing is.
        int n = f->kind == FD_TCP   ? tcpsock_read(f->tcp, bounce, MIN(len, BOUNCE))
                : f->kind == FD_PTY ? pty_master_read(f->tty, bounce, MIN(len, BOUNCE))
                                    : pipe_read(f->pipe, bounce, MIN(len, BOUNCE));
        if (n > 0 && copy_to_user(ubuf, bounce, (size_t)n)) n = -EFAULT;
        kfree(bounce);
        return n;
    }
    uint32_t done = 0;
    int err = 0;
    mutex_lock(&f->lock);
    while (done < len) {
        uint32_t chunk = MIN(len - done, BOUNCE);
        int n = diskfs_pread(f->node, bounce, chunk, f->offset);
        if (n <= 0) { err = n; break; }
        if (copy_to_user((uint8_t*)ubuf + done, bounce, (size_t)n)) { err = -EFAULT; break; }
        f->offset += (uint32_t)n;
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) break;  // end of file
    }
    mutex_unlock(&f->lock);
    kfree(bounce);
    return done ? (int)done : err;
}

int file_write(int fd, const void* ubuf, uint32_t len) {
    ofile_t* f = get_fd(fd);
    if (!f || (f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->kind == FD_SOCKET) return -EINVAL;  // and SYS_SENDTO
    if (len == 0) return 0;
    if (f->kind == FD_CHAN) {
        if (len > CHAN_MSG_MAX) return -EMSGSIZE;
        uint8_t msg[CHAN_MSG_MAX];
        if (copy_from_user(msg, ubuf, len)) return -EFAULT;
        return chan_write(f->chan, f->side, msg, len);
    }
    if (user_range_ok((uintptr_t)ubuf, len, 0)) return -EFAULT;

    uint8_t* bounce = kmalloc(BOUNCE);
    if (!bounce) return -ENOMEM;
    uint32_t done = 0;
    int err = 0;
    if (f->kind == FD_TTY) tty_lock_output(ftty(f));
    if (f->kind == FD_FILE) mutex_lock(&f->lock);
    while (done < len) {
        uint32_t chunk = MIN(len - done, BOUNCE);
        if (copy_from_user(bounce, (const uint8_t*)ubuf + done, chunk)) { err = -EFAULT; break; }
        if (f->kind == FD_PIPE_W || f->kind == FD_TCP || f->kind == FD_TTY || f->kind == FD_PTY) {
            int n = f->kind == FD_TCP   ? tcpsock_write(f->tcp, bounce, chunk)
                    : f->kind == FD_TTY ? tty_write(ftty(f), (const char*)bounce, chunk)
                    : f->kind == FD_PTY ? pty_master_write(f->tty, bounce, chunk)
                                        : pipe_write(f->pipe, bounce, chunk);
            if (n <= 0) { err = n; break; }
            done += (uint32_t)n;
            if ((uint32_t)n < chunk) break;  // the readers went away, or the connection or terminal did
            continue;
        }
        if (f->flags & O_APPEND) f->offset = diskfs_size(f->node);
        int n = diskfs_pwrite(f->node, bounce, chunk, f->offset);
        if (n <= 0) { err = n; break; }
        f->offset += (uint32_t)n;
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) break;  // disk full
    }
    if (f->kind == FD_FILE) mutex_unlock(&f->lock);
    if (f->kind == FD_TTY) tty_unlock_output(ftty(f));
    kfree(bounce);
    return done ? (int)done : err;
}

int file_seek(int fd, int32_t off, int whence) {
    ofile_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_FILE) return -EINVAL;  // consoles and pipes do not seek
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -EINVAL;
    mutex_lock(&f->lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->offset : (int64_t)diskfs_size(f->node);
    int64_t pos = base + off;
    int r = pos < 0 || pos > 0x7FFFFFFF ? -EINVAL : (int)pos;
    if (r >= 0) f->offset = (uint32_t)pos;
    mutex_unlock(&f->lock);
    return r;
}

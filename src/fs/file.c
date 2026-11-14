// Per-task file descriptors and current directory. A program starts
// with console input on fd 0 and console output on 1 and 2, or with
// copies of its parent's descriptors; the rest are diskfs files and
// pipe ends and sockets. It starts in its parent's directory. Copies of a
// file descriptor each keep their own offset. All buffers passed in are user
// pointers; data moves through a kernel bounce buffer, so the disk
// code never touches user memory.
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

#define BOUNCE 4096u

void files_init_std(task_t* t) {
    t->cwd = current ? current->cwd : DISKFS_ROOT;
    diskfs_ref(t->cwd);
    memset(t->fds, 0, sizeof t->fds);
    t->fds[STDIN_FILENO].kind = FD_CONSOLE_IN;
    t->fds[STDIN_FILENO].flags = O_RDONLY;
    t->fds[STDOUT_FILENO].kind = FD_CONSOLE_OUT;
    t->fds[STDOUT_FILENO].flags = O_WRONLY;
    t->fds[STDERR_FILENO] = t->fds[STDOUT_FILENO];
}

int file_fd_ok(int fd) {
    return fd >= 0 && fd < MAX_FDS && current->fds[fd].kind != FD_NONE;
}

static void fd_ref(const fd_t* f) {
    if (f->kind == FD_FILE) diskfs_ref(f->node);
    else if (f->kind == FD_PIPE_R || f->kind == FD_PIPE_W) pipe_ref(f->pipe, f->kind == FD_PIPE_W);
    else if (f->kind == FD_SOCKET) udp_socket_ref(f->sock);
    else if (f->kind == FD_TCP) tcpsock_ref(f->tcp);
}

static void fd_release(fd_t* f) {
    if (f->kind == FD_FILE) diskfs_unref(f->node);
    else if (f->kind == FD_PIPE_R || f->kind == FD_PIPE_W) pipe_unref(f->pipe, f->kind == FD_PIPE_W);
    else if (f->kind == FD_SOCKET) udp_socket_unref(f->sock);
    else if (f->kind == FD_TCP) tcpsock_unref(f->tcp);
    f->kind = FD_NONE;
}

void files_inherit(task_t* child, task_t* parent, const int std[3]) {
    for (int i = 0; i < 3; i++) {
        const fd_t* f = &parent->fds[std[i]];
        if (f->kind == FD_NONE) continue;  // the caller checked; keep the console
        fd_ref(f);
        child->fds[i] = *f;
    }
}

void files_close_all(task_t* t) {
    for (int i = 0; i < MAX_FDS; i++) fd_release(&t->fds[i]);
    diskfs_unref(t->cwd);
    t->cwd = DISKFS_ROOT;
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
        if (current->fds[i].kind == FD_NONE) return i;
    return -EMFILE;
}

static fd_t* get_fd(int fd) {
    if (fd < 0 || fd >= MAX_FDS || current->fds[fd].kind == FD_NONE) return 0;
    return &current->fds[fd];
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
        fd_t* t = &current->fds[fd];
        memset(t, 0, sizeof *t);
        t->kind = acc == O_RDONLY ? FD_CONSOLE_IN : FD_CONSOLE_OUT;
        t->flags = acc;
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
    if (flags & O_TRUNC) {
        int r = diskfs_truncate(node, 0);
        if (r < 0) { diskfs_unref(node); return r; }
    }
    fd_t* f = &current->fds[fd];
    f->kind = FD_FILE;
    f->flags = flags;
    f->node = node;
    f->offset = 0;
    return fd;
}

int file_close(int fd) {
    fd_t* f = get_fd(fd);
    if (!f) return -EBADF;
    fd_release(f);
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
    if (copy_to_user(ufds, fds, sizeof fds)) {
        pipe_unref(p, 0);
        pipe_unref(p, 1);
        return -EFAULT;
    }
    fd_t* r = &current->fds[fds[0]];
    fd_t* w = &current->fds[fds[1]];
    memset(r, 0, sizeof *r);
    memset(w, 0, sizeof *w);
    r->kind = FD_PIPE_R; r->flags = O_RDONLY; r->pipe = p;
    w->kind = FD_PIPE_W; w->flags = O_WRONLY; w->pipe = p;
    return 0;
}

int file_socket(int type) {
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -EINVAL;
    int fd = free_fd(0);
    if (fd < 0) return fd;
    fd_t* f = &current->fds[fd];
    if (type == SOCK_DGRAM) {
        struct socket* sk = udp_socket_new();
        if (!sk) return -ENFILE;
        memset(f, 0, sizeof *f);
        f->kind = FD_SOCKET;
        f->sock = sk;
    } else {
        struct tcp* c = tcpsock_new();
        if (!c) return -ENFILE;
        memset(f, 0, sizeof *f);
        f->kind = FD_TCP;
        f->tcp = c;
    }
    f->flags = O_RDWR;
    return fd;
}

int file_get_socket(int fd, struct socket** s) {
    fd_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind == FD_TCP) return -EOPNOTSUPP;
    if (f->kind != FD_SOCKET) return -ENOTSOCK;
    *s = f->sock;
    return 0;
}

int file_get_tcp(int fd, struct tcp** c) {
    fd_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind == FD_SOCKET) return -EOPNOTSUPP;
    if (f->kind != FD_TCP) return -ENOTSOCK;
    *c = f->tcp;
    return 0;
}

int file_accept(int fd, sock_addr_t* uaddr, int timeout_ms) {
    struct tcp* l;
    int r = file_get_tcp(fd, &l);
    if (r) return r;
    // Everything that could fail is checked before a connection is
    // taken, so none is lost.
    int nfd = free_fd(0);
    if (nfd < 0) return nfd;
    if (uaddr && user_range_ok((uintptr_t)uaddr, sizeof *uaddr, 1)) return -EFAULT;
    struct tcp* c;
    sock_addr_t a;
    uint32_t ip;
    r = tcpsock_accept(l, &c, &ip, &a.port, timeout_ms);
    if (r) return r;
    a.ip = ip;
    fd_t* f = &current->fds[nfd];
    memset(f, 0, sizeof *f);
    f->kind = FD_TCP;
    f->flags = O_RDWR;
    f->tcp = c;
    if (uaddr && copy_to_user(uaddr, &a, sizeof a)) return -EFAULT;  // the fd is open all the same
    return nfd;
}

int file_fdtype(int fd) {
    fd_t* f = get_fd(fd);
    if (!f) return -EBADF;
    switch (f->kind) {
    case FD_CONSOLE_IN:
    case FD_CONSOLE_OUT: return FDTYPE_TTY;
    case FD_FILE: return FDTYPE_FILE;
    case FD_PIPE_R:
    case FD_PIPE_W: return FDTYPE_PIPE;
    case FD_SOCKET: return FDTYPE_UDP;
    case FD_TCP: return FDTYPE_TCP;
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
    fd_t* f = get_fd(fd);
    if (!f) return POLLNVAL;
    switch (f->kind) {
    case FD_CONSOLE_IN: return current->job == tty_foreground() && tty_pending() ? POLLIN : 0;
    case FD_PIPE_R: return pipe_events(f->pipe, 0);
    case FD_PIPE_W: return pipe_events(f->pipe, 1);
    case FD_SOCKET: return udp_socket_events(f->sock);
    case FD_TCP: return tcpsock_events(f->tcp);
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
            p[i].revents = p[i].fd < 0 ? 0 : fd_events(p[i].fd) & (p[i].events | POLLERR | POLLHUP | POLLNVAL);
            count += p[i].revents != 0;
        }
        if (count || timeout_ms == 0) break;
        if (current->killed) return -EINTR;
        uint32_t now = timer_ticks(), wake = now + TIMER_HZ / 10;
        if (timeout_ms > 0) {
            if ((int32_t)(now - until) >= 0) break;
            if ((int32_t)(wake - until) > 0) wake = until;
        }
        uint32_t f = irq_save();
        if (poll_gen == gen && !current->killed) task_block_timeout((void*)&poll_gen, wake);
        irq_restore(f);
    }
    if (copy_to_user(ufds, p, size)) return -EFAULT;
    return count;
}

int file_dup2(int oldfd, int newfd) {
    fd_t* f = get_fd(oldfd);
    if (!f || newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    fd_t copy = *f;
    fd_ref(&copy);  // before releasing newfd, in case both share a pipe
    fd_release(&current->fds[newfd]);
    current->fds[newfd] = copy;
    return newfd;
}

static int console_read(void* ubuf, uint32_t len) {
    if (current->job != tty_foreground()) return -EIO;  // background job
    char buf[256];
    if (len > sizeof buf) len = sizeof buf;
    if (user_range_ok((uintptr_t)ubuf, len, 1)) return -EFAULT;
    int n = tty_read(buf, len);
    if (n > 0 && copy_to_user(ubuf, buf, (size_t)n)) return -EFAULT;
    return n;
}

int file_read(int fd, void* ubuf, uint32_t len) {
    fd_t* f = get_fd(fd);
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->kind == FD_SOCKET) return -EINVAL;  // UDP sockets take SYS_RECVFROM
    if (len == 0) return 0;
    if (f->kind == FD_CONSOLE_IN) return console_read(ubuf, len);
    if (f->kind != FD_FILE && f->kind != FD_PIPE_R && f->kind != FD_TCP) return -EBADF;
    if (user_range_ok((uintptr_t)ubuf, len, 1)) return -EFAULT;

    uint8_t* bounce = kmalloc(BOUNCE);
    if (!bounce) return -ENOMEM;
    if (f->kind == FD_PIPE_R || f->kind == FD_TCP) {
        // Whatever is there now, waiting only if nothing is.
        int n = f->kind == FD_TCP ? tcpsock_read(f->tcp, bounce, MIN(len, BOUNCE))
                                  : pipe_read(f->pipe, bounce, MIN(len, BOUNCE));
        if (n > 0 && copy_to_user(ubuf, bounce, (size_t)n)) n = -EFAULT;
        kfree(bounce);
        return n;
    }
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        uint32_t chunk = MIN(len - done, BOUNCE);
        int n = diskfs_pread(f->node, bounce, chunk, f->offset);
        if (n <= 0) { err = n; break; }
        if (copy_to_user((uint8_t*)ubuf + done, bounce, (size_t)n)) { err = -EFAULT; break; }
        f->offset += (uint32_t)n;
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) break;  // end of file
    }
    kfree(bounce);
    return done ? (int)done : err;
}

int file_write(int fd, const void* ubuf, uint32_t len) {
    fd_t* f = get_fd(fd);
    if (!f || (f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->kind == FD_SOCKET) return -EINVAL;  // and SYS_SENDTO
    if (len == 0) return 0;
    if (user_range_ok((uintptr_t)ubuf, len, 0)) return -EFAULT;

    uint8_t* bounce = kmalloc(BOUNCE);
    if (!bounce) return -ENOMEM;
    uint32_t done = 0;
    int err = 0;
    if (f->kind == FD_CONSOLE_OUT) console_lock();
    while (done < len) {
        uint32_t chunk = MIN(len - done, BOUNCE);
        if (copy_from_user(bounce, (const uint8_t*)ubuf + done, chunk)) { err = -EFAULT; break; }
        if (f->kind == FD_CONSOLE_OUT) {
            console_write((const char*)bounce, chunk);
            done += chunk;
            continue;
        }
        if (f->kind == FD_PIPE_W || f->kind == FD_TCP) {
            int n = f->kind == FD_TCP ? tcpsock_write(f->tcp, bounce, chunk) : pipe_write(f->pipe, bounce, chunk);
            if (n <= 0) { err = n; break; }
            done += (uint32_t)n;
            if ((uint32_t)n < chunk) break;  // the readers went away, or the connection did
            continue;
        }
        if (f->flags & O_APPEND) f->offset = diskfs_size(f->node);
        int n = diskfs_pwrite(f->node, bounce, chunk, f->offset);
        if (n <= 0) { err = n; break; }
        f->offset += (uint32_t)n;
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) break;  // disk full
    }
    if (f->kind == FD_CONSOLE_OUT) console_unlock();
    kfree(bounce);
    return done ? (int)done : err;
}

int file_seek(int fd, int32_t off, int whence) {
    fd_t* f = get_fd(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_FILE) return -EINVAL;  // consoles and pipes do not seek
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->offset; break;
    case SEEK_END: base = diskfs_size(f->node); break;
    default: return -EINVAL;
    }
    int64_t pos = base + off;
    if (pos < 0 || pos > 0x7FFFFFFF) return -EINVAL;
    f->offset = (uint32_t)pos;
    return (int)pos;
}

// System calls: int 0x80, with the number and three arguments in the
// registers interrupt.h names (eax, ebx, ecx, edx on i386) and the
// result back in the first (negative errno on failure). Arguments and
// results are register sized, so an address fits whatever the width.
// Interrupts are on while a call runs, so calls can block.
//
// Every pointer a program passes is a user pointer and goes through
// the checked copy routines in usermem.c. Nothing here dereferences
// one directly except gfx_blit, after user_range_ok has proven the
// whole buffer mapped and user readable.
#include "kernel.h"
#include "interrupt.h"
#include "task.h"
#include "fs.h"
#include "mm.h"
#include "tty.h"
#include "console.h"
#include "drivers.h"
#include "arch.h"
#include "net.h"
#include "random.h"
#include "ct.h"
#include "civil.h"
#include "timefloor.h"  // RINGTAIL_TIME_FLOOR, made by the Makefile
#include "ringtail/abi.h"

typedef long (*syscall_fn)(uintptr_t a, uintptr_t b, uintptr_t c);

static long sys_print(uintptr_t ubuf, uintptr_t len, uintptr_t c) {
    (void)c;
    if (user_range_ok(ubuf, len, 0)) return -EFAULT;
    char buf[256];
    uint32_t done = 0;
    console_lock();  // one call's output comes out in one piece
    while (done < len) {
        uint32_t n = MIN(len - done, sizeof buf);
        if (copy_from_user(buf, (const char*)(ubuf + done), n)) break;
        console_write(buf, n);
        done += n;
    }
    console_unlock();
    return (int)done;
}

static long sys_exit(uintptr_t status, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    task_exit((int)status);
}

static long sys_sleep(uintptr_t seconds, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (seconds > 86400) seconds = 86400;
    return task_sleep_ticks(seconds * TIMER_HZ);
}

static long sys_open(uintptr_t name, uintptr_t flags, uintptr_t c) {
    (void)c;
    return file_open((const char*)name, (int)flags);
}

static long sys_read(uintptr_t fd, uintptr_t buf, uintptr_t len) {
    return file_read((int)fd, (void*)buf, len);
}

static long sys_write(uintptr_t fd, uintptr_t buf, uintptr_t len) {
    return file_write((int)fd, (const void*)buf, len);
}

static long sys_close(uintptr_t fd, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return file_close((int)fd);
}

static int copy_path(char path[PATH_MAX], uintptr_t upath) {
    return strncpy_from_user(path, (const char*)upath, PATH_MAX);
}

static long sys_delete(uintptr_t upath, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    return n < 0 ? n : diskfs_unlink(file_cwd(), path);
}

static long sys_mkdir(uintptr_t upath, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    return n < 0 ? n : diskfs_mkdir(file_cwd(), path);
}

static long sys_rmdir(uintptr_t upath, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    return n < 0 ? n : diskfs_rmdir(file_cwd(), path);
}

static long sys_chdir(uintptr_t upath, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    return n < 0 ? n : file_chdir(path);
}

static long sys_getcwd(uintptr_t ubuf, uintptr_t size, uintptr_t c) {
    (void)c;
    char path[PATH_MAX];
    int n = diskfs_getcwd(file_cwd(), path, sizeof path);
    if (n < 0) return n;
    if ((uint32_t)n + 1 > size) return -ENAMETOOLONG;
    return copy_to_user((void*)ubuf, path, (uint32_t)n + 1) ? -EFAULT : n;
}

// Replace a whole file in one step: the new contents go to fresh
// clusters and the directory entry switches only once they are all
// written, so a full disk or an I/O error leaves the old file intact.
#define SAVE_MAX (1024u * 1024u)
static long sys_rename(uintptr_t ufrom, uintptr_t uto, uintptr_t c) {
    (void)c;
    char from[PATH_MAX], to[PATH_MAX];
    int n = copy_path(from, ufrom);
    if (n >= 0) n = copy_path(to, uto);
    return n < 0 ? n : diskfs_rename(file_cwd(), from, to);
}
static long sys_stat(uintptr_t upath, uintptr_t uent, uintptr_t c) {
    (void)c;
    char path[PATH_MAX];
    dirent_t d;
    int n = copy_path(path, upath);
    if (n >= 0) n = diskfs_stat(file_cwd(), path, &d);
    if (n < 0) return n;
    return copy_to_user((void*)uent, &d, sizeof d) ? -EFAULT : 0;
}
static long sys_time(uintptr_t ut, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    rtc_time_t t;
    rtc_read(&t);
    return copy_to_user((void*)ut, &t, sizeof t) ? -EFAULT : 0;
}
// The floor is the time of the last commit when the kernel was built:
// the code did not exist before then, so a clock set earlier is wrong.
static long sys_settime(uintptr_t ut, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    rtc_time_t t;
    if (copy_from_user(&t, (const void*)ut, sizeof t)) return -EFAULT;
    if (!civil_valid(&t)) return t.year > CIVIL_MAX_YEAR ? -ERANGE : -EINVAL;
    if (t.year > 2099 || civil_to_unix(&t) < RINGTAIL_TIME_FLOOR) return -ERANGE;
    rtc_write(&t);
    return 0;
}
static long sys_save(uintptr_t upath, uintptr_t ubuf, uintptr_t len) {
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    if (n < 0) return n;
    if (len > SAVE_MAX) return -EFBIG;
    if (user_range_ok(ubuf, len, 0)) return -EFAULT;
    void* buf = kmalloc(len ? len : 1);
    if (!buf) return -ENOMEM;
    int r = copy_from_user(buf, (const void*)ubuf, len) ? -EFAULT : diskfs_write_file(file_cwd(), path, buf, len);
    kfree(buf);
    return r;
}

// ---- Graphics: only the task that owns the display may draw ----

static int is_owner(void) { return gfx_owner() == current->id; }

static long sys_gfx_begin(uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; (void)b; (void)c;
    if (is_owner()) return 0;
    return gfx_acquire(current->id);
}

static long sys_gfx_end(uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; (void)b; (void)c;
    return gfx_release(current->id);
}

static long sys_gfx_pixel(uintptr_t x, uintptr_t y, uintptr_t color) {
    if (!is_owner()) return -EPERM;
    gfx_pixel((int)x, (int)y, (uint8_t)color);
    return 0;
}

static long sys_gfx_clear(uintptr_t color, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_clear((uint8_t)color);
    return 0;
}

static long sys_gfx_rect(uintptr_t uarg, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_rect_t r;
    if (copy_from_user(&r, (const void*)uarg, sizeof r)) return -EFAULT;
    gfx_rect(r.x, r.y, r.w, r.h, (uint8_t)r.color, r.filled);
    return 0;
}

static long sys_gfx_line(uintptr_t uarg, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_line_t l;
    if (copy_from_user(&l, (const void*)uarg, sizeof l)) return -EFAULT;
    gfx_line(l.x0, l.y0, l.x1, l.y1, (uint8_t)l.color);
    return 0;
}

static long sys_gfx_text(uintptr_t uarg, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_text_t t;
    if (copy_from_user(&t, (const void*)uarg, sizeof t)) return -EFAULT;
    char s[GFX_WIDTH / 8 + 1];  // more would be off screen anyway
    int n = strncpy_from_user(s, t.str, sizeof s);
    if (n == -ENAMETOOLONG) {
        s[sizeof s - 1] = 0;  // the copy filled the buffer: show what fits
    } else if (n < 0) {
        return n;
    }
    gfx_text(t.x, t.y, s, (uint8_t)t.color);
    return 0;
}

static long sys_gfx_blit(uintptr_t upix, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    if (user_range_ok(upix, GFX_WIDTH * GFX_HEIGHT, 0)) return -EFAULT;
    gfx_blit((const uint8_t*)upix);
    return 0;
}

static long sys_gfx_font(uintptr_t ubuf, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return copy_to_user((void*)ubuf, gfx_font(), 256 * 16);
}

// ---- Input ----

static long sys_mouse_state(uintptr_t uarg, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    mouse_state_t m;
    mouse_get(&m.x, &m.y, &m.buttons);
    return copy_to_user((void*)uarg, &m, sizeof m);
}

static long sys_key_get(uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    int k = keyboard_raw_get();
    return k < 0 ? -EAGAIN : k;
}

// Something new for the caller: the mouse differs from what it last
// saw, or a key is waiting for it.
static int input_ready(int lx, int ly, int lb) {
    int x, y, btn;
    mouse_get(&x, &y, &btn);
    if (x != lx || y != ly || btn != lb) return 1;
    if (is_owner()) return keyboard_raw_pending();
    return current->job == tty_foreground() && tty_pending();
}

static long sys_wait(uintptr_t lx, uintptr_t ly, uintptr_t lb) {
    uint32_t f = irq_save();
    int r = 0;
    while (!input_ready((int)lx, (int)ly, (int)lb)) {
        if (current->killed) { r = -EINTR; break; }
        task_block(&input_chan);
    }
    irq_restore(f);
    return r;
}

// Wait up to `ticks` for the terminal to have input for the caller (the
// foreground job), so a program can redraw on a timer and still react
// to a key at once. Capped at an hour. Nothing is read: the caller does
// that with SYS_READ. A program that is not in the foreground just
// sleeps the time out.
static long sys_poll(uintptr_t ticks, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (ticks > 3600u * TIMER_HZ) ticks = 3600u * TIMER_HZ;
    uint32_t end = timer_ticks() + ticks;
    for (;;) {
        if (current->job == tty_foreground() && tty_pending()) return 1;
        if (current->killed) return -EINTR;
        if ((int32_t)(timer_ticks() - end) >= 0) return 0;
        task_sleep_ticks(1);
    }
}

// One ICMP echo request and its reply (net.c does the work).
static long sys_ping(uintptr_t up, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    ping_t p;
    if (copy_from_user(&p, (const void*)up, sizeof p)) return -EFAULT;
    if (user_range_ok(up, sizeof p, 1)) return -EFAULT;  // fail before sending, not after
    int r = net_ping(&p);
    if (r) return r;
    return copy_to_user((void*)up, &p, sizeof p);
}

// ---- Sockets (udp.c, tcpsock.c) and the network's configuration (net.c) ----

static long sys_socket(uintptr_t type, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return file_socket((int)type);
}

static long sys_bind(uintptr_t fd, uintptr_t port, uintptr_t c) {
    (void)c;
    struct socket* s;
    struct tcp* t;
    if (file_get_tcp((int)fd, &t) == 0) return tcpsock_bind(t, (int)port);
    int r = file_get_socket((int)fd, &s);
    return r ? r : udp_bind(s, (int)port);
}

static long sys_listen(uintptr_t fd, uintptr_t backlog, uintptr_t c) {
    (void)c;
    struct tcp* t;
    int r = file_get_tcp((int)fd, &t);
    return r ? r : tcpsock_listen(t, (int)backlog);
}

static long sys_accept(uintptr_t fd, uintptr_t uaddr, uintptr_t timeout_ms) {
    return file_accept((int)fd, (sock_addr_t*)uaddr, (int)timeout_ms);
}

static long sys_connect(uintptr_t fd, uintptr_t uaddr, uintptr_t timeout_ms) {
    struct tcp* t;
    sock_addr_t a;
    int r = file_get_tcp((int)fd, &t);
    if (r) return r;
    if (copy_from_user(&a, (const void*)uaddr, sizeof a)) return -EFAULT;
    return tcpsock_connect(t, a.ip, a.port, (int)timeout_ms);
}

static long sys_shutdown(uintptr_t fd, uintptr_t how, uintptr_t c) {
    (void)c;
    struct tcp* t;
    int r = file_get_tcp((int)fd, &t);
    return r ? r : tcpsock_shutdown(t, (int)how);
}

// A UDP socket has only a port of its own: its address is any of ours.
static long sys_sockaddr(uintptr_t fd, uintptr_t peer, uintptr_t uaddr) {
    struct tcp* t;
    struct socket* s;
    sock_addr_t a;
    uint32_t ip = 0;
    int r;
    if (user_range_ok(uaddr, sizeof a, 1)) return -EFAULT;
    if (file_get_tcp((int)fd, &t) == 0) r = tcpsock_addr(t, peer != 0, &ip, &a.port);
    else if ((r = file_get_socket((int)fd, &s)) == 0) {
        if (peer) r = -ENOTCONN;
        else a.port = udp_socket_port(s);
    }
    if (r) return r;
    a.ip = ip;
    return copy_to_user((void*)uaddr, &a, sizeof a);
}

static long sys_pollfd(uintptr_t ufds, uintptr_t n, uintptr_t timeout_ms) {
    return file_pollfd((pollfd_t*)ufds, n, (int)timeout_ms);
}

static long sys_fdtype(uintptr_t fd, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return file_fdtype((int)fd);
}

static long sys_random(uintptr_t ubuf, uintptr_t len, uintptr_t c) {
    (void)c;
    uint8_t buf[RANDOM_MAX];
    if (len > sizeof buf) return -EINVAL;
    random_bytes(buf, len);
    int r = copy_to_user((void*)ubuf, buf, len) ? -EFAULT : (int)len;
    ct_wipe(buf, sizeof buf);  // they may be somebody's key
    return r;
}

static long sys_sendto(uintptr_t fd, uintptr_t umsg, uintptr_t c) {
    (void)c;
    struct socket* s;
    udp_msg_t m;
    int r = file_get_socket((int)fd, &s);
    if (r) return r;
    if (copy_from_user(&m, (const void*)umsg, sizeof m)) return -EFAULT;
    if (m.len < 0) return -EINVAL;
    if (m.len > UDP_MAX_SIZE) return -EMSGSIZE;
    uint8_t* buf = kmalloc(UDP_MAX_SIZE);
    if (!buf) return -ENOMEM;
    r = copy_from_user(buf, m.buf, (uint32_t)m.len) ? -EFAULT : udp_sendto(s, m.ip, m.port, buf, (uint32_t)m.len);
    kfree(buf);
    return r;
}

static long sys_recvfrom(uintptr_t fd, uintptr_t umsg, uintptr_t c) {
    (void)c;
    struct socket* s;
    udp_msg_t m;
    int r = file_get_socket((int)fd, &s);
    if (r) return r;
    if (copy_from_user(&m, (const void*)umsg, sizeof m)) return -EFAULT;
    if (m.len < 0) return -EINVAL;
    uint32_t size = MIN((uint32_t)m.len, UDP_MAX_SIZE);
    // Checked before a datagram is taken, so a bad buffer loses nothing.
    if (user_range_ok((uintptr_t)m.buf, size, 1) || user_range_ok(umsg, sizeof m, 1)) return -EFAULT;
    uint8_t* buf = kmalloc(UDP_MAX_SIZE);
    if (!buf) return -ENOMEM;
    uint32_t ip;
    int port;
    r = udp_recvfrom(s, &ip, &port, buf, size, m.timeout_ms);
    if (r >= 0) {
        m.ip = ip;
        m.port = port;
        if (copy_to_user(m.buf, buf, (uint32_t)r) || copy_to_user((void*)umsg, &m, sizeof m)) r = -EFAULT;
    }
    kfree(buf);
    return r;
}

static long sys_netconf(uintptr_t op, uintptr_t uc, uintptr_t c) {
    (void)c;
    netconf_t n;
    if (op == NETCONF_STATIC && copy_from_user(&n, (const void*)uc, sizeof n)) return -EFAULT;
    if (op == NETCONF_GET && user_range_ok(uc, sizeof n, 1)) return -EFAULT;
    int r = net_conf((int)op, &n);
    if (!r && op == NETCONF_GET && copy_to_user((void*)uc, &n, sizeof n)) r = -EFAULT;
    return r;
}

// ---- Files and tasks ----

static long sys_seek(uintptr_t fd, uintptr_t off, uintptr_t whence) {
    return file_seek((int)fd, (int32_t)off, (int)whence);
}

// Name of the index-th file (not directory) in the current directory.
// Restarts from the top on every call: slow for a big directory, but
// stateless.
static long sys_list(uintptr_t index, uintptr_t ubuf, uintptr_t size) {
    dirent_t d;
    int cursor = 0;
    for (uint32_t i = 0;; i++) {
        do {
            if (diskfs_readdir(file_cwd(), &cursor, &d) < 0) return -ENOENT;
        } while (d.flags & DIRENT_DIR);
        if (i == index) break;
    }
    uint32_t len = strlen(d.name) + 1;
    if (size < len) return -EINVAL;
    return copy_to_user((void*)ubuf, d.name, len);
}

// argv from user memory: up to ARGV_MAX strings, ARGS_MAX_BYTES in
// all, NULL-terminated. A NULL argv means just the program name.
#define SPAWN_ARGS_BYTES 3072

static int spawn_job(const spawn_opts_t* o) {
    if (o->flags & ~(SPAWN_NEW_JOB | SPAWN_JOIN | SPAWN_FOREGROUND)) return -EINVAL;
    if ((o->flags & (SPAWN_NEW_JOB | SPAWN_JOIN)) == (SPAWN_NEW_JOB | SPAWN_JOIN)) return -EINVAL;
    if (o->flags & SPAWN_NEW_JOB) return 0;
    if (o->flags & SPAWN_JOIN) return task_job_mine(o->job) ? o->job : -EPERM;
    return current->job;
}

static long sys_spawn(uintptr_t uname, uintptr_t uargv, uintptr_t uopts) {
    char name[PATH_MAX];
    int n = copy_path(name, uname);
    if (n < 0) return n;
    // While the caller holds the display the child could not print
    // anything visible, and could not take the display either.
    if (is_owner()) return -EBUSY;
    spawn_opts_t o = { 0, 0, { -1, -1, -1 } };
    if (uopts && copy_from_user(&o, (const void*)uopts, sizeof o)) return -EFAULT;
    int job = spawn_job(&o);
    if (job < 0) return job;
    int std[3];
    for (int i = 0; i < 3; i++) {
        std[i] = o.fd[i] < 0 ? i : o.fd[i];
        if (o.fd[i] >= 0 && !file_fd_ok(o.fd[i])) return -EBADF;
    }

    char* strings = kmalloc(SPAWN_ARGS_BYTES);
    if (!strings) return -ENOMEM;
    const char* argv[ARGV_MAX];
    int argc = 0, r = 0;
    uint32_t used = 0;
    if (!uargv) argv[argc++] = name;
    while (uargv) {
        uintptr_t up;
        if (copy_from_user(&up, (const void*)(uargv + (uintptr_t)argc * sizeof up), sizeof up)) { r = -EFAULT; break; }
        if (!up) break;
        if (argc == ARGV_MAX) { r = -E2BIG; break; }
        int len = strncpy_from_user(strings + used, (const char*)up, SPAWN_ARGS_BYTES - used);
        if (len == -ENAMETOOLONG) { r = -E2BIG; break; }
        if (len < 0) { r = len; break; }
        argv[argc++] = strings + used;
        used += (uint32_t)len + 1;
    }
    if (!r && argc == 0) r = -EINVAL;
    // A bare name is a program in the root; a path is taken as it is.
    int at = strchr(name, '/') ? file_cwd() : DISKFS_ROOT;
    int fg = (o.flags & SPAWN_FOREGROUND) != 0;
    if (!r && fg && current->job != tty_foreground()) r = -EPERM;  // only the terminal's owner can pass it on
    if (!r) r = loader_spawn(at, name, argc, argv, job, std, fg);
    kfree(strings);
    if (r > 0 && fg) {
        // The child gets the terminal before its first instruction.
        int h = tty_handover(job ? job : r);
        if (h) task_kill(r);  // never ran; it ends at once
        task_release(r);
        if (h) r = h;
    }
    return r;
}

static long sys_tty_fg(uintptr_t job, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return tty_handover((int)job);
}

static long sys_wait_job(uintptr_t job, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if ((int)job <= 0) return -EINVAL;
    task_wait_job((int)job);
    return current->killed ? -EINTR : 0;
}

static long sys_kill(uintptr_t id, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return task_kill_user((int)id);
}

static long sys_readdir(uintptr_t upath, uintptr_t ucursor, uintptr_t uent) {
    char path[PATH_MAX];
    int n = copy_path(path, upath);
    if (n < 0) return n;
    int cursor;
    if (copy_from_user(&cursor, (const void*)ucursor, sizeof cursor)) return -EFAULT;
    int dir = diskfs_opendir(file_cwd(), path);
    if (dir < 0) return dir;
    dirent_t d;
    memset(&d, 0, sizeof d);
    int r = diskfs_readdir(dir, &cursor, &d);
    diskfs_unref(dir);
    if (r == -ENOENT) return 0;  // the end; a missing directory failed above
    if (r < 0) return r;
    if (copy_to_user((void*)uent, &d, sizeof d)) return -EFAULT;
    return copy_to_user((void*)ucursor, &cursor, sizeof cursor) ? -EFAULT : 1;
}

#define KINFO_MAX 4096
static long sys_kinfo(uintptr_t what, uintptr_t ubuf, uintptr_t size) {
    char* buf = kmalloc(KINFO_MAX);
    if (!buf) return -ENOMEM;
    sbuf_t sb = { buf, KINFO_MAX, 0 };
    int r = kinfo_format((int)what, &sb);
    if (!r && size) {
        uint32_t n = MIN((uint32_t)MIN(sb.len, KINFO_MAX - 1), size - 1);
        buf[n] = 0;
        r = copy_to_user((void*)ubuf, buf, n + 1);
    }
    kfree(buf);
    return r ? r : (int)sb.len;
}

static long sys_set_color(uintptr_t color, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (color > 0xFF || (color >> 4) == (color & 0x0F)) return -EINVAL;  // invisible text
    console_set_color((uint8_t)color);
    return 0;
}

static long sys_pipe(uintptr_t ufds, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return file_pipe((int*)ufds);
}

static long sys_dup2(uintptr_t oldfd, uintptr_t newfd, uintptr_t c) {
    (void)c;
    return file_dup2((int)oldfd, (int)newfd);
}

static long sys_power(uintptr_t what, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (what == POWER_REBOOT) { kprintf("Rebooting...\n"); machine_reboot(); }
    if (what == POWER_OFF) { kprintf("Powering off...\n"); machine_poweroff(); }
    return -EINVAL;
}

// Move the program break. The pages between the old and new break
// are mapped at once, so the heap never takes a page fault. Shrinking
// moves the break but keeps the pages, which a later grow reuses.
static long sys_sbrk(uintptr_t incr, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    uintptr_t old = current->brk;
    int64_t want = (int64_t)old + (int32_t)incr;  // sbrk takes an int
    if (want < (int64_t)current->brk_start || want > (int64_t)USER_IMAGE_LIMIT) return -ENOMEM;
    uintptr_t nb = (uintptr_t)want;
    for (uintptr_t va = ALIGN_UP(old, PAGE_SIZE); va < ALIGN_UP(nb, PAGE_SIZE); va += PAGE_SIZE)
        if (!as_map(current->cr3, va, 1)) return -ENOMEM;
    current->brk = nb;
    return (long)old;
}

static long sys_tty_mode(uintptr_t mode, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return tty_set_mode((int)mode);
}

static long sys_ticks(uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; (void)b; (void)c;
    return (int)timer_ticks();
}

static long sys_getpid(uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)a; (void)b; (void)c;
    return current->id;
}

static long sys_join(uintptr_t id, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    return task_join((int)id);
}

static const syscall_fn table[NUM_SYSCALLS] = {
    [SYS_PRINT] = sys_print,
    [SYS_EXIT] = sys_exit,
    [SYS_SLEEP] = sys_sleep,
    [SYS_OPEN] = sys_open,
    [SYS_READ] = sys_read,
    [SYS_WRITE] = sys_write,
    [SYS_CLOSE] = sys_close,
    [SYS_DELETE] = sys_delete,
    [SYS_GFX_BEGIN] = sys_gfx_begin,
    [SYS_GFX_END] = sys_gfx_end,
    [SYS_GFX_PIXEL] = sys_gfx_pixel,
    [SYS_GFX_CLEAR] = sys_gfx_clear,
    [SYS_GFX_RECT] = sys_gfx_rect,
    [SYS_GFX_LINE] = sys_gfx_line,
    [SYS_GFX_TEXT] = sys_gfx_text,
    [SYS_MOUSE_STATE] = sys_mouse_state,
    [SYS_KEY_GET] = sys_key_get,
    [SYS_SEEK] = sys_seek,
    [SYS_LIST] = sys_list,
    [SYS_SPAWN] = sys_spawn,
    [SYS_WAIT] = sys_wait,
    [SYS_TICKS] = sys_ticks,
    [SYS_GETPID] = sys_getpid,
    [SYS_JOIN] = sys_join,
    [SYS_GFX_BLIT] = sys_gfx_blit,
    [SYS_GFX_FONT] = sys_gfx_font,
    [SYS_SBRK] = sys_sbrk,
    [SYS_TTY_MODE] = sys_tty_mode,
    [SYS_SAVE] = sys_save,
    [SYS_TTY_FG] = sys_tty_fg,
    [SYS_WAIT_JOB] = sys_wait_job,
    [SYS_KILL] = sys_kill,
    [SYS_READDIR] = sys_readdir,
    [SYS_KINFO] = sys_kinfo,
    [SYS_SET_COLOR] = sys_set_color,
    [SYS_POWER] = sys_power,
    [SYS_PIPE] = sys_pipe,
    [SYS_DUP2] = sys_dup2,
    [SYS_MKDIR] = sys_mkdir,
    [SYS_RMDIR] = sys_rmdir,
    [SYS_CHDIR] = sys_chdir,
    [SYS_GETCWD] = sys_getcwd,
    [SYS_RENAME] = sys_rename,
    [SYS_STAT] = sys_stat,
    [SYS_TIME] = sys_time,
    [SYS_SETTIME] = sys_settime,
    [SYS_POLL] = sys_poll,
    [SYS_PING] = sys_ping,
    [SYS_SOCKET] = sys_socket,
    [SYS_BIND] = sys_bind,
    [SYS_SENDTO] = sys_sendto,
    [SYS_RECVFROM] = sys_recvfrom,
    [SYS_NETCONF] = sys_netconf,
    [SYS_LISTEN] = sys_listen,
    [SYS_ACCEPT] = sys_accept,
    [SYS_CONNECT] = sys_connect,
    [SYS_SHUTDOWN] = sys_shutdown,
    [SYS_SOCKADDR] = sys_sockaddr,
    [SYS_POLLFD] = sys_pollfd,
    [SYS_FDTYPE] = sys_fdtype,
    [SYS_RANDOM] = sys_random,
};

void syscall_handle(regs_t* r) {
    uintptr_t n = SYSCALL_NR(r);
    if (n >= NUM_SYSCALLS || !table[n]) {
        SYSCALL_RET(r) = (uintptr_t)-ENOSYS;
        return;
    }
    SYSCALL_RET(r) = (uintptr_t)table[n](SYSCALL_ARG0(r), SYSCALL_ARG1(r), SYSCALL_ARG2(r));
}

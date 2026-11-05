// RingTail user library: system call wrappers and a few helpers.
// Wrappers return what the kernel returns: a result, or -errno.
#ifndef RINGTAIL_ULIB_H
#define RINGTAIL_ULIB_H

#include <stdint.h>
#include <stddef.h>
#include "ringtail/abi.h"
#include "lib.h"  // string functions, snprintf, strerror, parse_uint

static inline int syscall3(int n, uint32_t a, uint32_t b, uint32_t c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
#define SC0(n) syscall3((n), 0, 0, 0)
#define SC1(n, a) syscall3((n), (uint32_t)(a), 0, 0)
#define SC2(n, a, b) syscall3((n), (uint32_t)(a), (uint32_t)(b), 0)
#define SC3(n, a, b, c) syscall3((n), (uint32_t)(a), (uint32_t)(b), (uint32_t)(c))

static inline int sys_print(const char* s, uint32_t len) { return SC2(SYS_PRINT, s, len); }
static inline __attribute__((noreturn)) void sys_exit(int status) {
    SC1(SYS_EXIT, status);
    for (;;) { }
}
static inline int sys_sleep(uint32_t seconds) { return SC1(SYS_SLEEP, seconds); }
static inline int sys_open(const char* path, int flags) { return SC2(SYS_OPEN, path, flags); }
static inline int sys_read(int fd, void* buf, uint32_t len) { return SC3(SYS_READ, fd, buf, len); }
static inline int sys_write(int fd, const void* buf, uint32_t len) { return SC3(SYS_WRITE, fd, buf, len); }
static inline int sys_close(int fd) { return SC1(SYS_CLOSE, fd); }
static inline int sys_delete(const char* path) { return SC1(SYS_DELETE, path); }
static inline int sys_mkdir(const char* path) { return SC1(SYS_MKDIR, path); }
static inline int sys_rmdir(const char* path) { return SC1(SYS_RMDIR, path); }
static inline int sys_chdir(const char* path) { return SC1(SYS_CHDIR, path); }
static inline int sys_getcwd(char* buf, uint32_t size) { return SC2(SYS_GETCWD, buf, size); }
static inline int sys_rename(const char* from, const char* to) { return SC2(SYS_RENAME, from, to); }
static inline int sys_stat(const char* path, dirent_t* d) { return SC2(SYS_STAT, path, d); }
static inline int sys_time(rtc_time_t* t) { return SC1(SYS_TIME, t); }
static inline int sys_poll(uint32_t ticks) { return SC1(SYS_POLL, ticks); }
static inline int sys_ping(ping_t* p) { return SC1(SYS_PING, p); }
static inline int sys_socket(int type) { return SC1(SYS_SOCKET, type); }
static inline int sys_bind(int fd, int port) { return SC2(SYS_BIND, fd, port); }
static inline int sys_sendto(int fd, const udp_msg_t* m) { return SC2(SYS_SENDTO, fd, m); }
static inline int sys_recvfrom(int fd, udp_msg_t* m) { return SC2(SYS_RECVFROM, fd, m); }
static inline int sys_netconf(int op, netconf_t* c) { return SC2(SYS_NETCONF, op, c); }
static inline int sys_listen(int fd, int backlog) { return SC2(SYS_LISTEN, fd, backlog); }
static inline int sys_accept(int fd, sock_addr_t* a, int timeout_ms) { return SC3(SYS_ACCEPT, fd, a, timeout_ms); }
static inline int sys_connect(int fd, const sock_addr_t* a, int timeout_ms) { return SC3(SYS_CONNECT, fd, a, timeout_ms); }
static inline int sys_shutdown(int fd, int how) { return SC2(SYS_SHUTDOWN, fd, how); }
static inline int sys_sockaddr(int fd, int peer, sock_addr_t* a) { return SC3(SYS_SOCKADDR, fd, peer, a); }
static inline int sys_pollfd(pollfd_t* fds, int n, int timeout_ms) { return SC3(SYS_POLLFD, fds, n, timeout_ms); }
static inline int sys_fdtype(int fd) { return SC1(SYS_FDTYPE, fd); }
static inline int sys_seek(int fd, int32_t off, int whence) { return SC3(SYS_SEEK, fd, off, whence); }
static inline int sys_list(int index, char* buf, uint32_t size) { return SC3(SYS_LIST, index, buf, size); }
static inline int sys_spawn(const char* name) { return SC2(SYS_SPAWN, name, 0); }
// argv[0] is conventionally the program name; the list ends with NULL.
static inline int sys_spawn_args(const char* name, const char* const* argv) { return SC2(SYS_SPAWN, name, argv); }
static inline int sys_spawn_opts(const char* name, const char* const* argv, const spawn_opts_t* o) {
    return SC3(SYS_SPAWN, name, argv, o);
}
static inline int sys_tty_fg(int job) { return SC1(SYS_TTY_FG, job); }
static inline int sys_wait_job(int job) { return SC1(SYS_WAIT_JOB, job); }
static inline int sys_kill(int id) { return SC1(SYS_KILL, id); }
static inline int sys_readdir(const char* path, int* cursor, dirent_t* d) { return SC3(SYS_READDIR, path, cursor, d); }
static inline int sys_kinfo(int what, char* buf, uint32_t size) { return SC3(SYS_KINFO, what, buf, size); }
static inline int sys_set_color(int attr) { return SC1(SYS_SET_COLOR, attr); }
static inline int sys_power(int what) { return SC1(SYS_POWER, what); }
static inline int sys_pipe(int fds[2]) { return SC1(SYS_PIPE, fds); }
static inline int sys_dup2(int oldfd, int newfd) { return SC2(SYS_DUP2, oldfd, newfd); }
static inline int sys_tty_mode(int mode) { return SC1(SYS_TTY_MODE, mode); }
static inline int sys_save(const char* path, const void* buf, uint32_t len) { return SC3(SYS_SAVE, path, buf, len); }
static inline int sys_join(int id) { return SC1(SYS_JOIN, id); }
static inline int sys_getpid(void) { return SC0(SYS_GETPID); }
static inline uint32_t sys_ticks(void) { return (uint32_t)SC0(SYS_TICKS); }

static inline int sys_gfx_begin(void) { return SC0(SYS_GFX_BEGIN); }
static inline int sys_gfx_end(void) { return SC0(SYS_GFX_END); }
static inline int sys_gfx_pixel(int x, int y, int color) { return SC3(SYS_GFX_PIXEL, x, y, color); }
static inline int sys_gfx_clear(int color) { return SC1(SYS_GFX_CLEAR, color); }
static inline int sys_gfx_rect(const gfx_rect_t* r) { return SC1(SYS_GFX_RECT, r); }
static inline int sys_gfx_line(const gfx_line_t* l) { return SC1(SYS_GFX_LINE, l); }
static inline int sys_gfx_text(const gfx_text_t* t) { return SC1(SYS_GFX_TEXT, t); }
static inline int sys_gfx_blit(const uint8_t* pixels) { return SC1(SYS_GFX_BLIT, pixels); }
static inline int sys_gfx_font(uint8_t* buf) { return SC1(SYS_GFX_FONT, buf); }
static inline int sys_mouse_state(mouse_state_t* m) { return SC1(SYS_MOUSE_STATE, m); }
static inline int sys_key_get(void) { return SC0(SYS_KEY_GET); }
static inline int sys_wait(int x, int y, int buttons) { return SC3(SYS_WAIT, x, y, buttons); }

// printf("addr " IPV4_FMT "\n", IPV4_ARGS(ip)) for an address in host order.
#define IPV4_FMT "%u.%u.%u.%u"
#define IPV4_ARGS(ip) (unsigned)(ip) >> 24, ((unsigned)(ip) >> 16) & 255, ((unsigned)(ip) >> 8) & 255, (unsigned)(ip) & 255
static inline int mask_bits(unsigned m) {
    int bits = 0;
    for (; m & 0x80000000u; m <<= 1) bits++;
    return bits;
}

// Memory. sbrk returns the old break, or NULL when memory runs out.
void* sbrk(int incr);
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t size);
void free(void* p);

// Output to stdout.
int print(const char* s);
int puts(const char* s);  // adds a newline
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// For mv and cp: where src goes when dst is given. If dst is an
// existing directory, that is dst/NAME with src's last name; else dst
// itself. 0, or -ENAMETOOLONG if it does not fit out.
int dest_path(const char* src, const char* dst, char* out, size_t size);

// Self-checking programs: check() prints one result line, and
// check_summary() prints the total and returns an exit status.
int check(int ok, const char* what);
int check_summary(const char* program);

#endif

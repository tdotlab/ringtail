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
static inline int sys_open(const char* name, int flags) { return SC2(SYS_OPEN, name, flags); }
static inline int sys_read(int fd, void* buf, uint32_t len) { return SC3(SYS_READ, fd, buf, len); }
static inline int sys_write(int fd, const void* buf, uint32_t len) { return SC3(SYS_WRITE, fd, buf, len); }
static inline int sys_close(int fd) { return SC1(SYS_CLOSE, fd); }
static inline int sys_delete(const char* name) { return SC1(SYS_DELETE, name); }
static inline int sys_seek(int fd, int32_t off, int whence) { return SC3(SYS_SEEK, fd, off, whence); }
static inline int sys_list(int index, char* buf, uint32_t size) { return SC3(SYS_LIST, index, buf, size); }
static inline int sys_spawn(const char* name) { return SC1(SYS_SPAWN, name); }
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

// Output to stdout.
int print(const char* s);
int puts(const char* s);  // adds a newline
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Self-checking programs: check() prints one result line, and
// check_summary() prints the total and returns an exit status.
int check(int ok, const char* what);
int check_summary(const char* program);

#endif

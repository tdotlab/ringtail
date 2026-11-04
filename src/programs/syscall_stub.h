// Program-side syscall stubs, shared by the programs in this
// folder. These are the program's half of the int 0x80 doorway:
// the kernel's half is syscall.c, and the numbers must match it
// exactly (PRINT 1, EXIT 2, SLEEP 3, OPEN 4, READ 5, WRITE 6,
// CLOSE 7, DELETE 8, graphics and input 9-17). A program never
// calls a kernel function directly; it only asks through this
// interrupt, with the syscall number in eax and arguments in
// ebx, ecx, edx.
// The wrapper is one inline asm statement: move the number and
// arguments into registers, int $0x80, take the result from eax.
// Programs are freestanding (no libc), so they also carry their
// own string helpers where they need them, in each program file.
#include <stdint.h>

#define SYS_PRINT 1
#define SYS_EXIT 2
#define SYS_SLEEP 3
#define SYS_OPEN 4
#define SYS_READ 5
#define SYS_WRITE 6
#define SYS_CLOSE 7
#define SYS_DELETE 8
#define SYS_GFX_BEGIN 9
#define SYS_GFX_END 10
#define SYS_GFX_PIXEL 11
#define SYS_GFX_CLEAR 12
#define SYS_GFX_RECT 13
#define SYS_GFX_LINE 14
#define SYS_GFX_TEXT 15
#define SYS_MOUSE_STATE 16
#define SYS_KEY_GET 17

static inline int do_syscall(int num, int a, int b, int c) {
    int result;
    // The "memory" clobber is load-bearing, not boilerplate.
    // Several syscalls take a pointer to a struct (a rectangle,
    // a text request, a mouse output struct), and the kernel
    // reads or writes that struct's memory during the interrupt.
    // Without "memory", the compiler is allowed to keep a
    // program's struct-field writes in registers, or reorder
    // them after this asm, because it cannot see that the
    // interrupt touches that memory: it only sees a pointer
    // value going into ebx. That is exactly what the first GUI
    // run showed: pixels (plain numbers in registers) drew, and
    // rectangles and text (struct pointers) drew nothing,
    // because the kernel read unwritten stack. "memory" forces
    // every pending write out before the interrupt, and forces
    // a reload of anything the kernel may have written, after it.
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(num), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return result;
}

static inline void sys_print(const char* s, int len) {
    do_syscall(SYS_PRINT, (int)s, len, 0);
}

static inline void sys_exit(void) {
    do_syscall(SYS_EXIT, 0, 0, 0);
    while (1) { } // exit never returns; this is just a backstop
}

static inline void sys_sleep(int seconds) {
    do_syscall(SYS_SLEEP, seconds, 0, 0);
}

// File syscalls, v0.7.2. Open takes a null-terminated filename
// and returns a per-task descriptor (0-7) or -1. Read and write
// take a descriptor, a buffer, and a length (capped at 4096 by
// the kernel), and return bytes transferred or -1. Read advances
// the offset and returns 0 at end of file. Close takes a
// descriptor and returns 0 or -1. Delete takes a filename and
// returns 0 or -1; it does not close descriptors already open
// for that file, and later operations on those descriptors fail.
static inline int sys_open(const char* name) {
    return do_syscall(SYS_OPEN, (int)name, 0, 0);
}

static inline int sys_read(int fd, char* buf, int len) {
    return do_syscall(SYS_READ, fd, (int)buf, len);
}

static inline int sys_write(int fd, const char* buf, int len) {
    return do_syscall(SYS_WRITE, fd, (int)buf, len);
}

static inline int sys_close(int fd) {
    return do_syscall(SYS_CLOSE, fd, 0, 0);
}

static inline int sys_delete(const char* name) {
    return do_syscall(SYS_DELETE, (int)name, 0, 0);
}

// Graphics and input syscalls, v0.9.0. The kernel provides
// primitives, not a toolkit: a program draws pixels, rectangles,
// lines, and text, reads the mouse, and reads keys, and builds
// its own windows and buttons out of those, in its own code.
// Multi-argument calls take a pointer to a struct, because the
// doorway only carries three register arguments, and a rectangle
// needs six values. The structs are all ints, in the order the
// kernel reads them, so there is no padding to disagree about.
// Colors are palette indices 0-15, the same palette the kernel
// demo uses. Drawing calls only work for the program that owns
// graphics mode (it called GFX_BEGIN and has not called GFX_END
// yet); any other program gets -1, so two programs cannot fight
// over one framebuffer. MOUSE_STATE is the exception: reading
// the mouse changes nothing, so any program may call it.
// KEY_GET returns the next keyboard scancode, or -1 if the queue
// is empty, and only the graphics owner may call it, because the
// shell normally drains that queue, and two drainers would race
// (the kernel main loop stops draining while graphics is owned,
// which is what makes KEY_GET safe for the owner and only the
// owner).
typedef struct { int x, y, w, h, color, filled; } gfx_rect_t;
typedef struct { int x0, y0, x1, y1, color; } gfx_line_t;
typedef struct { int x, y; const char* str; int color; } gfx_text_t;
typedef struct { int x, y, buttons; } mouse_state_t;

static inline int sys_gfx_begin(void) {
    return do_syscall(SYS_GFX_BEGIN, 0, 0, 0);
}
static inline int sys_gfx_end(void) {
    return do_syscall(SYS_GFX_END, 0, 0, 0);
}
static inline int sys_gfx_pixel(int x, int y, int color) {
    return do_syscall(SYS_GFX_PIXEL, x, y, color);
}
static inline int sys_gfx_clear(int color) {
    return do_syscall(SYS_GFX_CLEAR, color, 0, 0);
}
static inline int sys_gfx_rect(const gfx_rect_t* r) {
    return do_syscall(SYS_GFX_RECT, (int)r, 0, 0);
}
static inline int sys_gfx_line(const gfx_line_t* l) {
    return do_syscall(SYS_GFX_LINE, (int)l, 0, 0);
}
static inline int sys_gfx_text(const gfx_text_t* t) {
    return do_syscall(SYS_GFX_TEXT, (int)t, 0, 0);
}
static inline int sys_mouse_state(mouse_state_t* m) {
    return do_syscall(SYS_MOUSE_STATE, (int)m, 0, 0);
}
static inline int sys_key_get(void) {
    return do_syscall(SYS_KEY_GET, 0, 0, 0);
}

static inline int prog_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// Text console: the VGA text screen (80x25), mirrored to the serial
// port so the system can be driven and tested headless.
#include "console.h"
#include "kernel.h"
#include "arch.h"
#include "sync.h"
#include "drivers.h"

#define COLS 80
#define ROWS 25

static uint16_t* const VGA = (uint16_t*)0xB8000;
static uint16_t shadow[COLS * ROWS];
static uint16_t* screen = VGA;
static int detached;
static int row, col;
static uint8_t color = COLOR_DEFAULT;
static mutex_t lock = MUTEX_INIT;
static volatile int panic_mode;

void console_lock(void) { if (!panic_mode) mutex_lock(&lock); }
void console_unlock(void) { if (!panic_mode) mutex_unlock(&lock); }

static void update_cursor(void) {
    if (detached) return;
    uint16_t pos = (uint16_t)(row * COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
}

static void scroll(void) {
    memmove(screen, screen + COLS, (ROWS - 1) * COLS * 2);
    for (int i = 0; i < COLS; i++) screen[(ROWS - 1) * COLS + i] = (uint16_t)(' ' | (color << 8));
    row = ROWS - 1;
}

static void put_raw(char c) {
    switch (c) {
    case '\n': col = 0; row++; break;
    case '\r': col = 0; break;
    case '\b': if (col > 0) col--; break;
    case '\t': col = (col + 8) & ~7; if (col >= COLS) { col = 0; row++; } break;
    default:
        if ((uint8_t)c < 0x20) return;
        screen[row * COLS + col] = (uint16_t)((uint8_t)c | (color << 8));
        if (++col == COLS) { col = 0; row++; }
    }
    if (row >= ROWS) scroll();
}

static void serial_mirror(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') serial_putc('\r');
        serial_putc(s[i]);
    }
}

void console_init(void) {
    console_clear();
}

void console_clear(void) {
    console_lock();
    for (int i = 0; i < COLS * ROWS; i++) screen[i] = (uint16_t)(' ' | (color << 8));
    row = col = 0;
    update_cursor();
    console_unlock();
}

void console_set_color(uint8_t c) { color = c; }
uint8_t console_get_color(void) { return color; }

void console_write(const char* s, size_t len) {
    console_lock();
    for (size_t i = 0; i < len; i++) put_raw(s[i]);
    update_cursor();
    serial_mirror(s, len);
    console_unlock();
}

void console_puts(const char* s) { console_write(s, strlen(s)); }
void console_putc(char c) { console_write(&c, 1); }

void console_puts_color(const char* s, uint8_t c) {
    console_lock();
    uint8_t old = color;
    color = c;
    console_write(s, strlen(s));
    color = old;
    console_unlock();
}

static int vkprintf_color(int use_color, uint8_t c, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    console_lock();
    uint8_t old = color;
    if (use_color) color = c;
    console_write(buf, len);
    color = old;
    console_unlock();
    return n;
}

int kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vkprintf_color(0, 0, fmt, ap);
    va_end(ap);
    return n;
}

int kprintf_color(uint8_t c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vkprintf_color(1, c, fmt, ap);
    va_end(ap);
    return n;
}

void console_backspace(void) {
    console_lock();
    if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = COLS - 1;
    }
    screen[row * COLS + col] = (uint16_t)(' ' | (color << 8));
    update_cursor();
    serial_write("\b \b", 3);
    console_unlock();
}

void console_detach(void) {
    console_lock();
    if (!detached) {
        memcpy(shadow, VGA, sizeof(shadow));
        screen = shadow;
        detached = 1;
    }
    console_unlock();
}

void console_attach(void) {
    console_lock();
    if (detached) {
        memcpy(VGA, shadow, sizeof(shadow));
        screen = VGA;
        detached = 0;
        update_cursor();
    }
    console_unlock();
}

void console_panic_mode(void) {
    panic_mode = 1;
}

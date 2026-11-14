// Text console: the VGA text screen (80x25), mirrored to the serial
// port so the system can be driven and tested headless.
#include "console.h"
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "sync.h"
#include "drivers.h"

#define COLS 80
#define ROWS 25

#define VGA ((uint16_t*)phys_to_virt(0xB8000))
static uint16_t shadow[COLS * ROWS];
static uint16_t* screen;  // VGA, or shadow while detached; set by console_init
static int detached;
static int row, col;
static int wrap_pending;        // wrote the last column; wrap on the next character
static int saved_row, saved_col;
static int cursor_hidden;
static uint8_t base_color = COLOR_DEFAULT;  // what console_set_color chose
static uint8_t color = COLOR_DEFAULT;       // base plus any SGR changes
static int reverse;
static int bold;  // SGR 1, kept apart from a bright default colour
static mutex_t lock = MUTEX_INIT;
static volatile int panic_mode;

// Escape sequence parser state.
enum { ST_PLAIN, ST_ESC, ST_CSI };
static int esc_state, esc_private, esc_n;
static int esc_params[8];

void console_lock(void) { if (!panic_mode) mutex_lock(&lock); }
void console_unlock(void) { if (!panic_mode) mutex_unlock(&lock); }

static void update_cursor(void) {
    if (detached) return;
    uint16_t pos = (uint16_t)(row * COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
    // Bit 5 of the cursor start register turns the cursor off.
    outb(0x3D4, 0x0A);
    uint8_t start = inb(0x3D5);
    outb(0x3D5, cursor_hidden ? (start | 0x20) : (start & ~0x20));
}

static uint16_t blank(void) { return (uint16_t)(' ' | (color << 8)); }

static void scroll(void) {
    memmove(screen, screen + COLS, (ROWS - 1) * COLS * 2);
    for (int i = 0; i < COLS; i++) screen[(ROWS - 1) * COLS + i] = blank();
    row = ROWS - 1;
}

static void clear_cells(int from, int to) {
    for (int i = from; i < to; i++) screen[i] = blank();
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ANSI colour numbers run black, red, green, yellow, blue, magenta,
// cyan, white; VGA puts blue first.
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void sgr(int p) {
    uint8_t fg = color & 0x0F, bg = (color >> 4) & 0x0F;
    if (reverse) { uint8_t t = fg; fg = bg; bg = t; }
    if (p == 0) { fg = base_color & 0x0F; bg = (base_color >> 4) & 0x0F; reverse = 0; bold = 0; }
    else if (p == 1) { bold = 1; fg |= 0x08; }
    else if (p == 22) { bold = 0; fg &= 0x07; }
    else if (p == 7) reverse = 1;
    else if (p == 27) reverse = 0;
    else if (p >= 30 && p <= 37) fg = (uint8_t)((bold ? 0x08 : 0) | ansi_to_vga[p - 30]);
    else if (p == 39) fg = base_color & 0x0F;
    else if (p >= 40 && p <= 47) bg = ansi_to_vga[p - 40];
    else if (p == 49) bg = (base_color >> 4) & 0x0F;
    else if (p >= 90 && p <= 97) fg = (uint8_t)(0x08 | ansi_to_vga[p - 90]);
    else if (p >= 100 && p <= 107) bg = (uint8_t)(0x08 | ansi_to_vga[p - 100]);
    if (reverse) { uint8_t t = fg; fg = bg; bg = t; }
    color = (uint8_t)((bg << 4) | fg);
}

static void csi(char final) {
    int n = esc_n;
    int p0 = n > 0 ? esc_params[0] : 0;
    int count = p0 ? p0 : 1;
    wrap_pending = 0;
    if (esc_private) {
        if (p0 == 25 && final == 'l') cursor_hidden = 1;
        if (p0 == 25 && final == 'h') cursor_hidden = 0;
        return;
    }
    switch (final) {
    case 'A': row = clampi(row - count, 0, ROWS - 1); break;
    case 'B': row = clampi(row + count, 0, ROWS - 1); break;
    case 'C': col = clampi(col + count, 0, COLS - 1); break;
    case 'D': col = clampi(col - count, 0, COLS - 1); break;
    case 'G': col = clampi(count - 1, 0, COLS - 1); break;
    case 'H': case 'f':
        row = clampi((p0 ? p0 : 1) - 1, 0, ROWS - 1);
        col = clampi((n > 1 && esc_params[1] ? esc_params[1] : 1) - 1, 0, COLS - 1);
        break;
    case 'J':
        if (p0 == 0) clear_cells(row * COLS + col, COLS * ROWS);
        else if (p0 == 1) clear_cells(0, row * COLS + col + 1);
        else if (p0 == 2) clear_cells(0, COLS * ROWS);
        break;
    case 'K':
        if (p0 == 0) clear_cells(row * COLS + col, row * COLS + COLS);
        else if (p0 == 1) clear_cells(row * COLS, row * COLS + col + 1);
        else if (p0 == 2) clear_cells(row * COLS, row * COLS + COLS);
        break;
    case 'm':
        if (n == 0) sgr(0);
        for (int i = 0; i < n; i++) sgr(esc_params[i]);
        break;
    case 's': saved_row = row; saved_col = col; break;
    case 'u': row = saved_row; col = saved_col; break;
    }
}

// Returns 1 if c was consumed by an escape sequence.
static int escape(char c) {
    if (esc_state == ST_ESC) {
        if (c == '[') {
            esc_state = ST_CSI;
            esc_private = 0;
            esc_n = 0;
            esc_params[0] = 0;
        } else {
            esc_state = ST_PLAIN;  // ESC c (reset) and others: ignored
        }
        return 1;
    }
    if (esc_state == ST_CSI) {
        if (c == '?' && esc_n == 0) {
            esc_private = 1;
        } else if (c >= '0' && c <= '9') {
            if (esc_n == 0) esc_n = 1;
            int* p = &esc_params[esc_n - 1];
            if (*p < 1000) *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_n == 0) esc_n = 1;
            if (esc_n < 8) esc_params[esc_n++] = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            esc_state = ST_PLAIN;
            csi(c);
        } else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x3F) {
            esc_state = ST_PLAIN;  // malformed: give up on it
        }
        return 1;
    }
    if (c == 0x1B) {
        esc_state = ST_ESC;
        return 1;
    }
    return 0;
}

static void newline(void) {
    col = 0;
    if (++row >= ROWS) scroll();
}

static void put_raw(char c) {
    if (escape(c)) return;
    switch (c) {
    case '\n': wrap_pending = 0; newline(); return;
    case '\r': wrap_pending = 0; col = 0; return;
    case '\b':
        // Back over the last character written, even across a line
        // wrap, so "\b \b" erases it wherever it is. After the last
        // column the cursor is still on that character.
        if (wrap_pending) wrap_pending = 0;
        else if (col > 0) col--;
        else if (row > 0) { row--; col = COLS - 1; }
        return;
    case '\t':
        if (wrap_pending) { wrap_pending = 0; newline(); }
        col = (col + 8) & ~7;
        if (col >= COLS) { col = COLS - 1; wrap_pending = 1; }
        return;
    }
    if ((uint8_t)c < 0x20) return;
    // Like a VT100, writing the last column leaves the cursor there and
    // wraps only when another character follows, so a program can fill
    // the bottom-right cell without scrolling the screen.
    if (wrap_pending) { wrap_pending = 0; newline(); }
    screen[row * COLS + col] = (uint16_t)((uint8_t)c | (color << 8));
    if (col == COLS - 1) wrap_pending = 1;
    else col++;
}

static void serial_mirror(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') serial_putc('\r');
        serial_putc(s[i]);
    }
}

void console_init(void) {
    screen = VGA;
    console_clear();
}

void console_clear(void) {
    console_lock();
    clear_cells(0, COLS * ROWS);
    row = col = 0;
    wrap_pending = 0;
    update_cursor();
    console_unlock();
}

void console_reset(void) {
    console_lock();
    color = base_color;
    reverse = 0;
    bold = 0;
    cursor_hidden = 0;
    esc_state = ST_PLAIN;
    clear_cells(0, COLS * ROWS);
    row = col = 0;
    wrap_pending = 0;
    update_cursor();
    static const char reset_seq[] = "\x1b[0m\x1b[?25h\x1b[2J\x1b[H";
    serial_write(reset_seq, sizeof reset_seq - 1);
    console_unlock();
}

void console_set_color(uint8_t c) { base_color = color = c; reverse = bold = 0; }
uint8_t console_get_color(void) { return base_color; }

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
    if (wrap_pending) {
        wrap_pending = 0;  // the cursor is still on the last character
    } else if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = COLS - 1;
    }
    screen[row * COLS + col] = blank();
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

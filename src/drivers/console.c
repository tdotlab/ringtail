// The console: a grid of character cells, mirrored to the serial port
// so the system can be driven and tested headless.
//
// On VGA text the grid is the card's own memory at 0xB8000, 80x25,
// and the card draws it. On a framebuffer (kernel.c says which the
// bootloader left) the grid is an array here, as big as the screen has
// room for in 8x16 characters (128x48 at 1024x768), and this file draws
// it: each cell's glyph from font8x16.c, in the 16 VGA colours. A cell
// is the same 16 bits either way, character and attribute, so the rest
// of the console does not care which.
//
// Drawing a whole screen of pixels takes far longer than changing a few
// cells, so on a framebuffer drawing waits: a write changes the grid and
// wakes a kernel task (console_start), which draws the cells that differ
// from what is on the screen, then waits a couple of ticks before it
// looks again. A program printing thousands of lines scrolls the grid
// thousands of times but the screen only as often as that. Before the
// task runs, and after a panic, writes draw at once.
//
// The drawing does not take the console lock, only its own (draw_lock,
// which attach and detach also take). A writer holds the console lock
// while it mirrors to the serial port, which goes at the UART's pace;
// the screen must not wait for that. Drawing reads each cell once and
// remembers what it drew, so a cell that changes meanwhile is simply
// still different next time.
#include "console.h"
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "sync.h"
#include "drivers.h"
#include "task.h"

#define VGA_COLS 80
#define VGA_ROWS 25
#define MAX_COLS 240  // 1920 pixels
#define MAX_ROWS 100  // 1600
#define GLYPH_W 8
#define GLYPH_H 16

extern const uint8_t font8x16[256 * 16];

#define VGA ((uint16_t*)phys_to_virt(0xB8000))
static int COLS = VGA_COLS, ROWS = VGA_ROWS;
// The grid, on a framebuffer. Not static: the QEMU tests find it by name
// (nm) and read the screen from it, as they read 0xB8000 on VGA text.
uint16_t console_cells[MAX_COLS * MAX_ROWS];
static uint16_t shadow[VGA_COLS * VGA_ROWS];  // VGA text while graphics has the card
static uint16_t* screen;  // VGA, shadow or cells; set by console_init
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

// ---- Drawing on a framebuffer ----
static int on_fb;                  // the grid is cells, drawn here
static volatile uint8_t* fb;       // mapped by console_map; 0 until then
static uint32_t fb_pitch, fb_bytes;  // bytes a row, bytes a pixel
static int left, top;              // where the grid starts, to centre it
static uint32_t pal[16];           // the VGA colours, as pixels
static uint16_t shown[MAX_COLS * MAX_ROWS];  // what the screen shows
static int shown_ok;               // 0: draw everything next time
static int shown_cursor = -1;      // the cell drawn with the cursor in it
static volatile int dirty;         // the grid changed since it was drawn
static int drawer;                 // the task that draws is running
static int draw_chan;
static mutex_t draw_lock = MUTEX_INIT;

// The 16 colours, as the VGA's DAC has them by default.
static const uint8_t vga_rgb[16][3] = {
    { 0, 0, 0 },       { 0, 0, 170 },     { 0, 170, 0 },     { 0, 170, 170 },
    { 170, 0, 0 },     { 170, 0, 170 },   { 170, 85, 0 },    { 170, 170, 170 },
    { 85, 85, 85 },    { 85, 85, 255 },   { 85, 255, 85 },   { 85, 255, 255 },
    { 255, 85, 85 },   { 255, 85, 255 },  { 255, 255, 85 },  { 255, 255, 255 },
};

uint32_t fb_pixel(uint8_t r, uint8_t g, uint8_t b) {
    const framebuffer_t* f = &boot_info.fb;
    return (uint32_t)(r >> (8 - f->red_bits)) << f->red_pos | (uint32_t)(g >> (8 - f->green_bits)) << f->green_pos |
           (uint32_t)(b >> (8 - f->blue_bits)) << f->blue_pos;
}

static inline void put_pixel(volatile uint8_t* p, uint32_t v) {
    switch (fb_bytes) {
    case 4: *(volatile uint32_t*)p = v; break;
    case 3: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(volatile uint16_t*)p = (uint16_t)v; break;
    }
}

// One cell. VGA attributes: low nibble foreground, high nibble
// background (all 16, since bit 7 means bright, not blinking). The
// cursor is a bar across the bottom two rows of its cell.
static void draw_cell(int i, uint16_t v, int cursor) {
    const uint8_t* g = &font8x16[(v & 0xFF) * GLYPH_H];
    uint32_t fg = pal[(v >> 8) & 0x0F], bg = pal[(v >> 12) & 0x0F];
    int x = left + (i % COLS) * GLYPH_W, y = top + (i / COLS) * GLYPH_H;
    volatile uint8_t* p = fb + (uint32_t)y * fb_pitch + (uint32_t)x * fb_bytes;
    for (int r = 0; r < GLYPH_H; r++, p += fb_pitch) {
        uint8_t bits = cursor && r >= GLYPH_H - 2 ? 0xFF : g[r];
        if (fb_bytes == 4) {
            volatile uint32_t* q = (volatile uint32_t*)p;
            for (int c = 0; c < GLYPH_W; c++) q[c] = bits & (0x80 >> c) ? fg : bg;
        } else {
            for (int c = 0; c < GLYPH_W; c++) put_pixel(p + c * fb_bytes, bits & (0x80 >> c) ? fg : bg);
        }
    }
}

static void fill(int x, int y, int w, int h, uint32_t v) {
    for (int r = 0; r < h; r++) {
        volatile uint8_t* p = fb + (uint32_t)(y + r) * fb_pitch + (uint32_t)x * fb_bytes;
        for (int c = 0; c < w; c++, p += fb_bytes) put_pixel(p, v);
    }
}

// Bring the screen up to date with the grid: the cells that changed,
// and the cursor. Called with draw_lock held (see draw_now).
static void draw(void) {
    __atomic_store_n(&dirty, 0, __ATOMIC_SEQ_CST);  // before the cells are read, even on another CPU
    if (!fb || detached) return;
    int n = COLS * ROWS;
    if (!shown_ok) {
        const framebuffer_t* f = &boot_info.fb;
        int gw = COLS * GLYPH_W, gh = ROWS * GLYPH_H;
        fill(0, 0, (int)f->width, top, pal[0]);
        fill(0, top + gh, (int)f->width, (int)f->height - top - gh, pal[0]);
        fill(0, top, left, gh, pal[0]);
        fill(left + gw, top, (int)f->width - left - gw, gh, pal[0]);
    }
    int r = row, c = col;  // once each: a writer may be moving them
    int cur = cursor_hidden ? -1 : r * COLS + c;
    for (int i = 0; i < n; i++) {
        uint16_t v = ((volatile uint16_t*)console_cells)[i];
        if (shown_ok && v == shown[i] && i != cur && i != shown_cursor) continue;
        draw_cell(i, v, i == cur);
        shown[i] = v;
    }
    shown_cursor = cur;
    shown_ok = 1;
}

static void draw_now(void) {
    if (!panic_mode) mutex_lock(&draw_lock);
    draw();
    if (!panic_mode) mutex_unlock(&draw_lock);
}

// The grid changed: draw it now, or have the drawing task do it soon.
static void changed(void) {
    if (!on_fb) return;
    if (drawer && !panic_mode) {
        dirty = 1;
        task_wakeup(&draw_chan);
    } else {
        draw_now();
    }
}

static void draw_task(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t f = irq_save();
        while (!dirty) task_block(&draw_chan);
        irq_restore(f);
        draw_now();
        task_sleep_ticks(2);  // let more output pile up before the next
    }
}

// Escape sequence parser state.
enum { ST_PLAIN, ST_ESC, ST_CSI };
static int esc_state, esc_private, esc_n;
static int esc_params[8];

void console_lock(void) { if (!panic_mode) mutex_lock(&lock); }
void console_unlock(void) { if (!panic_mode) mutex_unlock(&lock); }

static void update_cursor(void) {
    if (on_fb) {
        changed();
        return;
    }
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
    const framebuffer_t* f = &boot_info.fb;
    if (f->addr) {
        on_fb = 1;
        COLS = MIN((int)(f->width / GLYPH_W), MAX_COLS);
        ROWS = MIN((int)(f->height / GLYPH_H), MAX_ROWS);
        left = ((int)f->width - COLS * GLYPH_W) / 2;
        top = ((int)f->height - ROWS * GLYPH_H) / 2;
        fb_pitch = f->pitch;
        fb_bytes = (f->bpp + 7u) / 8;
        for (int i = 0; i < 16; i++) pal[i] = fb_pixel(vga_rgb[i][0], vga_rgb[i][1], vga_rgb[i][2]);
        screen = console_cells;
    } else {
        screen = VGA;
    }
    console_clear();
}

// The framebuffer is past the end of RAM, where nothing is mapped until
// paging is up: until this runs, the grid fills but nothing is drawn.
void console_map(void) {
    const framebuffer_t* f = &boot_info.fb;
    if (!on_fb) return;
    void* p = mmio_map_wc((uintptr_t)f->addr, f->pitch * f->height);
    if (!p) {
        static const char msg[] = "console: no room to map the framebuffer\r\n";
        serial_write(msg, sizeof msg - 1);
        return;
    }
    console_lock();
    mutex_lock(&draw_lock);
    fb = p;
    shown_ok = 0;
    draw();
    mutex_unlock(&draw_lock);
    console_unlock();
}

void console_start(void) {
    if (!on_fb || !fb) return;
    if (task_create_kernel("console", draw_task, 0) >= 0) drawer = 1;
}

int console_size(int* cols, int* rows) {
    *cols = COLS;
    *rows = ROWS;
    return 0;
}

const char* console_describe(char* buf, size_t size) {
    const framebuffer_t* f = &boot_info.fb;
    if (on_fb) snprintf(buf, size, "%ux%u, %u bits a pixel, %dx%d text", f->width, f->height, f->bpp, COLS, ROWS);
    else snprintf(buf, size, "VGA text, %dx%d", COLS, ROWS);
    return buf;
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

// On a framebuffer the grid stays where it is; only drawing stops, and
// starts again from scratch, since graphics drew over all of it.
void console_detach(void) {
    console_lock();
    if (!detached) {
        if (!on_fb) {
            memcpy(shadow, VGA, sizeof(shadow));
            screen = shadow;
        }
        mutex_lock(&draw_lock);  // not in the middle of a draw
        detached = 1;
        mutex_unlock(&draw_lock);
    }
    console_unlock();
}

void console_attach(void) {
    console_lock();
    if (detached) {
        if (!on_fb) {
            memcpy(VGA, shadow, sizeof(shadow));
            screen = VGA;
        }
        mutex_lock(&draw_lock);
        detached = 0;
        shown_ok = 0;
        mutex_unlock(&draw_lock);
        update_cursor();
    }
    console_unlock();
}

void console_panic_mode(void) {
    panic_mode = 1;
}

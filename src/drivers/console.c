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
#include "vt.h"

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
static int detached;
// The grid and what is written into it: the cells are VGA's, the
// shadow's or console_cells (vt.h).
static vt_t vt;
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
    int r = vt.row, c = vt.col;  // once each: a writer may be moving them
    int cur = vt.cursor_hidden ? -1 : r * COLS + c;
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

void console_lock(void) { if (!panic_mode) mutex_lock(&lock); }
void console_unlock(void) { if (!panic_mode) mutex_unlock(&lock); }

static void update_cursor(void) {
    if (on_fb) {
        changed();
        return;
    }
    if (detached) return;
    uint16_t pos = (uint16_t)(vt.row * COLS + vt.col);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
    // Bit 5 of the cursor start register turns the cursor off.
    outb(0x3D4, 0x0A);
    uint8_t start = inb(0x3D5);
    outb(0x3D5, vt.cursor_hidden ? (start | 0x20) : (start & ~0x20));
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
        vt_init(&vt, console_cells, COLS, ROWS, COLOR_DEFAULT);
    } else {
        vt_init(&vt, VGA, COLS, ROWS, COLOR_DEFAULT);
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
    vt_clear(&vt);
    update_cursor();
    console_unlock();
}

void console_reset(void) {
    console_lock();
    vt_reset(&vt);
    update_cursor();
    static const char reset_seq[] = "\x1b[0m\x1b[?25h\x1b[2J\x1b[H";
    serial_write(reset_seq, sizeof reset_seq - 1);
    console_unlock();
}

void console_set_color(uint8_t c) { vt_set_color(&vt, c); }
uint8_t console_get_color(void) { return vt.base_color; }

void console_write(const char* s, size_t len) {
    console_lock();
    vt_write(&vt, s, len);
    update_cursor();
    serial_mirror(s, len);
    console_unlock();
}

void console_puts(const char* s) { console_write(s, strlen(s)); }
void console_putc(char c) { console_write(&c, 1); }

void console_puts_color(const char* s, uint8_t c) {
    console_lock();
    uint8_t old = vt.color;
    vt.color = c;
    console_write(s, strlen(s));
    vt.color = old;
    console_unlock();
}

static int vkprintf_color(int use_color, uint8_t c, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    console_lock();
    uint8_t old = vt.color;
    if (use_color) vt.color = c;
    console_write(buf, len);
    vt.color = old;
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
    vt_erase_back(&vt);
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
            vt.cells = shadow;
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
            vt.cells = VGA;
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

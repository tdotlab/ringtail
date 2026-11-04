// VGA mode 13h graphics: 320x200, 256 colours, one byte per pixel
// at 0xA0000. The mode is set by programming the VGA registers
// directly (BIOS calls need real mode), and undone by restoring
// every register, the palette and the text font, which mode 13h
// overwrites because it uses the same video memory planes.
//
// One task at a time owns the display. While it does, the console
// writes to an off-screen copy of the text screen, so nothing
// printed meanwhile is lost, and the keyboard delivers raw scancodes
// to the owner instead of characters to the terminal.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "ringtail/abi.h"

#define W GFX_WIDTH
#define H GFX_HEIGHT
static volatile uint8_t* const FB = (volatile uint8_t*)0xA0000;

// ---- Register tables for mode 13h (the standard published values) ----
static const uint8_t seq_13h[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const uint8_t crtc_13h[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28,
    0x40, 0x96, 0xB9, 0xA3, 0xFF
};
static const uint8_t gc_13h[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF };
static const uint8_t attr_13h[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};
#define MISC_13H 0x63

// The 16 text-mode colours, as 6-bit DAC values, so colour numbers
// mean the same thing in both modes.
static const uint8_t palette16[16 * 3] = {
    0, 0, 0,     0, 0, 42,    0, 42, 0,    0, 42, 42,
    42, 0, 0,    42, 0, 42,   42, 21, 0,   42, 42, 42,
    21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
    63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63
};

static uint8_t save_misc;
static uint8_t save_seq[5], save_crtc[25], save_gc[9], save_attr[21];
static uint8_t save_palette[256 * 3];
// Font plane: 256 glyphs in 32-byte slots, 16 rows used, bit 7 is
// the leftmost pixel. Read at boot, written back after graphics.
static uint8_t font_plane[256 * 32];
static uint8_t font16[256 * 16];  // compact copy handed to programs
static int font_loaded;

static volatile int owner = -1;
static int active;

// ---- Register access ----
// The attribute controller shares one port for index and data; a
// read of 0x3DA resets it to "index next".
static void ac_write(uint8_t idx, uint8_t val) {
    (void)inb(0x3DA);
    outb(0x3C0, idx);
    outb(0x3C0, val);
}
static uint8_t ac_read(uint8_t idx) {
    (void)inb(0x3DA);
    outb(0x3C0, idx);
    return inb(0x3C1);
}
// Writing the attribute registers blanks the screen until palette
// access is re-enabled (index write with bit 5 set).
static void ac_enable_palette(void) {
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
}

static void write_seq(const uint8_t* t) {
    // Hold the sequencer in reset while its clocking changes.
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    for (int i = 1; i < 5; i++) { outb(0x3C4, (uint8_t)i); outb(0x3C5, t[i]); }
    outb(0x3C4, 0); outb(0x3C5, t[0]);
}
static void write_crtc(const uint8_t* t) {
    // CRTC 0-7 are write protected while 0x11 bit 7 is set.
    outb(0x3D4, 0x11); outb(0x3D5, 0x00);
    for (int i = 0; i < 25; i++) { outb(0x3D4, (uint8_t)i); outb(0x3D5, t[i]); }
}
static void write_gc(const uint8_t* t) {
    for (int i = 0; i < 9; i++) { outb(0x3CE, (uint8_t)i); outb(0x3CF, t[i]); }
}
static void write_attr(const uint8_t* t) {
    for (int i = 0; i < 21; i++) ac_write((uint8_t)i, t[i]);
    ac_enable_palette();
}

static void save_state(void) {
    save_misc = inb(0x3CC);
    for (int i = 0; i < 5; i++) { outb(0x3C4, (uint8_t)i); save_seq[i] = inb(0x3C5); }
    for (int i = 0; i < 25; i++) { outb(0x3D4, (uint8_t)i); save_crtc[i] = inb(0x3D5); }
    for (int i = 0; i < 9; i++) { outb(0x3CE, (uint8_t)i); save_gc[i] = inb(0x3CF); }
    for (int i = 0; i < 21; i++) save_attr[i] = ac_read((uint8_t)i);
    ac_enable_palette();
    outb(0x3C7, 0);
    for (int i = 0; i < 256 * 3; i++) save_palette[i] = inb(0x3C9);
}

static void restore_state(void) {
    outb(0x3C2, save_misc);
    write_seq(save_seq);
    write_crtc(save_crtc);
    write_gc(save_gc);
    write_attr(save_attr);
    outb(0x3C8, 0);
    for (int i = 0; i < 256 * 3; i++) outb(0x3C9, save_palette[i]);
}

// Direct CPU access to one memory plane. Text mode's odd/even
// addressing must be off, the memory map must point at 0xA0000, and
// the sequencer map mask selects planes as a bitmask (plane 2 is
// 0x04) while the graphics read map takes a plane number.
static uint8_t pa_mode, pa_misc, pa_map, pa_mem;
static void plane_begin(uint8_t plane) {
    outb(0x3CE, 5); pa_mode = inb(0x3CF);
    outb(0x3C4, 2); pa_map = inb(0x3C5);
    outb(0x3CE, 6); pa_misc = inb(0x3CF);
    outb(0x3CE, 5); outb(0x3CF, 0x00);
    outb(0x3C4, 4); pa_mem = inb(0x3C5);
    outb(0x3C4, 4); outb(0x3C5, pa_mem | 0x04);  // sequential, not odd/even
    outb(0x3C4, 2); outb(0x3C5, (uint8_t)(1 << plane));
    outb(0x3CE, 4); outb(0x3CF, plane);
    outb(0x3CE, 6); outb(0x3CF, (uint8_t)(pa_misc & ~0x0E) | 0x04);  // A0000, 64K, no chain
}
static void plane_end(void) {
    outb(0x3CE, 4); outb(0x3CF, 0x00);
    outb(0x3C4, 2); outb(0x3C5, pa_map);
    outb(0x3CE, 5); outb(0x3CF, pa_mode);
    outb(0x3CE, 6); outb(0x3CF, pa_misc);
    outb(0x3C4, 4); outb(0x3C5, pa_mem);
}

static void load_font(void) {
    plane_begin(2);
    for (int i = 0; i < 256 * 32; i++) font_plane[i] = FB[i];
    plane_end();
    for (int c = 0; c < 256; c++)
        for (int r = 0; r < 16; r++) font16[c * 16 + r] = font_plane[c * 32 + r];
    font_loaded = 1;
}

static void restore_font(void) {
    plane_begin(2);
    for (int i = 0; i < 256 * 32; i++) FB[i] = font_plane[i];
    plane_end();
}

const uint8_t* gfx_font(void) {
    return font16;
}

static void enter_graphics(void) {
    if (!font_loaded) load_font();
    console_detach();
    save_state();
    outb(0x3C2, MISC_13H);
    write_seq(seq_13h);
    write_crtc(crtc_13h);
    write_gc(gc_13h);
    write_attr(attr_13h);
    outb(0x3C8, 0);
    for (int i = 0; i < 16 * 3; i++) outb(0x3C9, palette16[i]);
    keyboard_raw_flush();
    active = 1;
    gfx_clear(1);
}

static void leave_graphics(void) {
    if (!active) return;
    active = 0;
    restore_state();
    restore_font();
    console_attach();
}

void gfx_init_font(void) {
    load_font();
}

// ---- Ownership ----

int gfx_acquire(int task_id) {
    uint32_t f = irq_save();
    int r = 0;
    if (owner == task_id) r = 0;
    else if (owner != -1) r = -EBUSY;
    else owner = task_id;
    irq_restore(f);
    if (r == 0 && !active) enter_graphics();
    return r;
}

int gfx_release(int task_id) {
    if (owner != task_id) return -EPERM;
    leave_graphics();
    keyboard_raw_flush();
    owner = -1;
    return 0;
}

int gfx_owner(void) { return owner; }

void gfx_task_exit(int task_id) {
    if (owner == task_id) gfx_release(task_id);
}

void gfx_emergency_text(void) {
    if (active) leave_graphics();
    owner = -1;
}

// ---- Drawing. Every primitive clips through gfx_pixel or does its
// own bounds check, so nothing can write outside the framebuffer.

void gfx_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    FB[y * W + x] = color;
}

void gfx_clear(uint8_t color) {
    memset((void*)FB, color, W * H);
}

void gfx_blit(const uint8_t* pixels) {
    memcpy((void*)FB, pixels, W * H);
}

void gfx_line(int x0, int y0, int x1, int y1, uint8_t color) {
    // Bresenham. Endpoints are clamped to a generous box first so a
    // huge coordinate cannot make the loop run for billions of steps.
    x0 = MAX(-W, MIN(2 * W, x0)); x1 = MAX(-W, MIN(2 * W, x1));
    y0 = MAX(-H, MIN(2 * H, y0)); y1 = MAX(-H, MIN(2 * H, y1));
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(int x, int y, int w, int h, uint8_t color, int filled) {
    if (w <= 0 || h <= 0) return;
    if (!filled) {
        gfx_line(x, y, x + w - 1, y, color);
        gfx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
        gfx_line(x, y, x, y + h - 1, color);
        gfx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
        return;
    }
    int x0 = MAX(0, x), y0 = MAX(0, y);
    int x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    if (x + w < x || y + h < y) { x1 = W; y1 = H; }  // overflowed: clip to the screen
    for (int yy = y0; yy < y1; yy++)
        if (x1 > x0) memset((void*)(FB + yy * W + x0), color, (size_t)(x1 - x0));
}

void gfx_text(int x, int y, const char* s, uint8_t color) {
    for (; *s && x < W; s++, x += 9) {
        const uint8_t* g = &font16[(uint8_t)*s * 16];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) gfx_pixel(x + col, y + row, color);
    }
}

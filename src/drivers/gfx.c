// Graphics for programs. One task at a time owns the display, in one
// of two modes:
//
//   classic   320x200 in 256 colours (the first 16 are the text colours),
//             drawn with the SYS_GFX_* primitives or a whole-screen blit.
//             This is VGA mode 13h, and what the first programs were
//             written for.
//   native    the screen's own size, in 24-bit colour: the program draws
//             into memory of its own and hands rectangles of 0xRRGGBB
//             pixels to SYS_GFX_PUT.
//
// What the screen really is decides how each is done. On a framebuffer
// (the usual case: GRUB sets one up), native writes straight to it, and
// classic draws into a 320x200 copy here that is shown enlarged as
// much as fits (3x on 1024x768) in the middle of the screen. On VGA
// text, both switch the card to mode 13h by programming its registers
// (BIOS calls need real mode), and native is 320x200 with a palette of
// 3 bits of red, 3 of green and 2 of blue. Leaving graphics restores
// every register, the palette and the text font, which mode 13h
// overwrites because it uses the same video memory planes.
//
// While a task owns the display, the console keeps its text but stops
// drawing it, and the keyboard delivers raw scancodes to the owner
// instead of characters to the terminal.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "ringtail/abi.h"

#define W GFX_WIDTH
#define H GFX_HEIGHT
#define VGA_FB ((volatile uint8_t*)phys_to_virt(0xA0000))

extern const uint8_t font8x16[256 * 16];

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
// the leftmost pixel. Read at boot, written back after mode 13h.
static uint8_t font_plane[256 * 32];

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
    for (int i = 0; i < 256 * 32; i++) font_plane[i] = VGA_FB[i];
    plane_end();
}

static void restore_font(void) {
    plane_begin(2);
    for (int i = 0; i < 256 * 32; i++) VGA_FB[i] = font_plane[i];
    plane_end();
}


// ---- State ----

static volatile int owner = -1;
static int mode;         // GFX_CLASSIC or GFX_NATIVE, while there is an owner
static int active;       // the screen shows graphics
static int on_fb;        // the screen is boot_info.fb
static volatile uint8_t* fb;
static uint32_t fb_w, fb_h, fb_pitch, fb_bytes;
static int scale, ox, oy;            // classic on a framebuffer: S times, at ox, oy
static uint8_t classic[W * H];       // classic on a framebuffer
static uint32_t classic_pal[256];    // its colours, as framebuffer pixels

static uint32_t pixel_rgb(uint32_t rgb) { return fb_pixel((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb); }

static inline void put_px(volatile uint8_t* p, uint32_t v) {
    switch (fb_bytes) {
    case 4: *(volatile uint32_t*)p = v; break;
    case 3: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(volatile uint16_t*)p = (uint16_t)v; break;
    }
}

static void fb_fill(int x, int y, int w, int h, uint32_t v) {
    for (int r = 0; r < h; r++) {
        volatile uint8_t* p = fb + (uint32_t)(y + r) * fb_pitch + (uint32_t)x * fb_bytes;
        for (int c = 0; c < w; c++, p += fb_bytes) put_px(p, v);
    }
}

// The 256 classic colours: the 16 text colours, 16 greys, then a 6x6x6
// cube of the rest, as 0xRRGGBB.
static uint32_t classic_rgb(int i) {
    static const uint32_t text16[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    if (i < 16) return text16[i];
    if (i < 32) {
        uint32_t g = (uint32_t)(i - 16) * 17;
        return g << 16 | g << 8 | g;
    }
    if (i < 32 + 216) {
        int k = i - 32;
        uint32_t r = (uint32_t)(k / 36) * 51, g = (uint32_t)(k / 6 % 6) * 51, b = (uint32_t)(k % 6) * 51;
        return r << 16 | g << 8 | b;
    }
    return 0;
}

// ---- Classic on a framebuffer: show part of the 320x200 copy ----

static void present(int x0, int y0, int x1, int y1) {
    if (!active || !on_fb || mode != GFX_CLASSIC) return;
    x0 = MAX(0, x0), y0 = MAX(0, y0), x1 = MIN(W, x1), y1 = MIN(H, y1);
    for (int y = y0; y < y1; y++) {
        const uint8_t* src = &classic[y * W];
        for (int s = 0; s < scale; s++) {
            volatile uint8_t* p = fb + (uint32_t)(oy + y * scale + s) * fb_pitch + (uint32_t)(ox + x0 * scale) * fb_bytes;
            if (fb_bytes == 4) {
                volatile uint32_t* q = (volatile uint32_t*)p;
                for (int x = x0; x < x1; x++) {
                    uint32_t v = classic_pal[src[x]];
                    for (int k = 0; k < scale; k++) *q++ = v;
                }
            } else {
                for (int x = x0; x < x1; x++)
                    for (int k = 0; k < scale; k++, p += fb_bytes) put_px(p, classic_pal[src[x]]);
            }
        }
    }
}

// ---- Entering and leaving graphics ----

static void enter_mode13h(int native) {
    console_detach();
    save_state();
    outb(0x3C2, MISC_13H);
    write_seq(seq_13h);
    write_crtc(crtc_13h);
    write_gc(gc_13h);
    write_attr(attr_13h);
    outb(0x3C8, 0);
    if (native) {
        // 3-3-2: red in the top bits, then green, then blue.
        for (int i = 0; i < 256; i++) {
            outb(0x3C9, (uint8_t)((i >> 5) * 63 / 7));
            outb(0x3C9, (uint8_t)((i >> 2 & 7) * 63 / 7));
            outb(0x3C9, (uint8_t)((i & 3) * 63 / 3));
        }
    } else {
        for (int i = 0; i < 16 * 3; i++) outb(0x3C9, palette16[i]);
    }
}

static void enter_graphics(void) {
    keyboard_raw_flush();
    if (on_fb) {
        console_detach();
        fb_fill(0, 0, (int)fb_w, (int)fb_h, 0);
        active = 1;
        if (mode == GFX_CLASSIC) gfx_clear(1);
        return;
    }
    enter_mode13h(mode == GFX_NATIVE);
    active = 1;
    if (mode == GFX_CLASSIC) gfx_clear(1);
    else memset((void*)VGA_FB, 0, W * H);
}

static void leave_graphics(void) {
    if (!active) return;
    active = 0;
    if (!on_fb) {
        restore_state();
        restore_font();
    }
    console_attach();
}

void gfx_init(void) {
    const framebuffer_t* f = &boot_info.fb;
    if (f->addr) {
        // The console has mapped it already; mapping it again costs only
        // page tables, and keeps the two drivers apart.
        fb = mmio_map_wc((uintptr_t)f->addr, f->pitch * f->height);
        if (fb) {
            on_fb = 1;
            fb_w = f->width, fb_h = f->height, fb_pitch = f->pitch, fb_bytes = (f->bpp + 7u) / 8;
            scale = (int)MIN(fb_w / W, fb_h / H);
            ox = ((int)fb_w - W * scale) / 2;
            oy = ((int)fb_h - H * scale) / 2;
            for (int i = 0; i < 256; i++) classic_pal[i] = pixel_rgb(classic_rgb(i));
            mouse_set_range((int)fb_w, (int)fb_h);
            return;
        }
    }
    mouse_set_range(W, H);
    load_font();
    // Attribute bit 7 means a bright background, not blinking text, so
    // ANSI's bright and reversed colours look right. Graphics mode
    // saves and restores this along with the other text registers.
    ac_write(0x10, ac_read(0x10) & ~0x08);
    ac_enable_palette();
}

const uint8_t* gfx_font(void) { return font8x16; }

void gfx_size(int how, int* w, int* h) {
    if (how == GFX_NATIVE && on_fb) {
        *w = (int)fb_w;
        *h = (int)fb_h;
    } else {
        *w = W;
        *h = H;
    }
}

// The mouse in the owner's terms: the screen's pixels for native, or
// classic's 320x200, which on a framebuffer is the whole screen scaled
// down, so the pointer can still reach every edge.
void gfx_mouse(int* x, int* y, int* buttons) {
    mouse_get(x, y, buttons);
    if (on_fb && !(owner >= 0 && mode == GFX_NATIVE)) {
        *x = *x * W / (int)fb_w;
        *y = *y * H / (int)fb_h;
    }
}

// ---- Ownership ----

int gfx_acquire(int task_id, int how) {
    if (how != GFX_CLASSIC && how != GFX_NATIVE) return -EINVAL;
    uint32_t f = irq_save();
    int r = 0;
    if (owner == task_id) r = how == mode ? 0 : -EBUSY;  // one mode until it ends
    else if (owner != -1) r = -EBUSY;
    else owner = task_id, mode = how;
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
int gfx_mode(void) { return mode; }

void gfx_task_exit(int task_id) {
    if (owner == task_id) gfx_release(task_id);
}

void gfx_emergency_text(void) {
    if (active) leave_graphics();
    owner = -1;
}

// ---- Classic drawing. Every primitive clips through gfx_pixel or does
// its own bounds check, so nothing can write outside the picture. On a
// framebuffer they draw into the copy and show what they changed.

static volatile uint8_t* classic_px(void) { return on_fb ? classic : VGA_FB; }

void gfx_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    classic_px()[y * W + x] = color;
    present(x, y, x + 1, y + 1);
}

void gfx_clear(uint8_t color) {
    memset((void*)classic_px(), color, W * H);
    present(0, 0, W, H);
}

void gfx_blit(const uint8_t* pixels) {
    memcpy((void*)classic_px(), pixels, W * H);
    present(0, 0, W, H);
}

// a + n for a span a program gave (n > 0), held at INT_MAX instead of
// wrapping: x and w both come from the program, and a signed int that
// wraps is undefined, so a check after the fact can be compiled away.
static int end_of(int a, int n) {
    int64_t e = (int64_t)a + n;
    return e > 0x7FFFFFFF ? 0x7FFFFFFF : (int)e;
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
    int xe = end_of(x, w), ye = end_of(y, h);
    if (!filled) {
        gfx_line(x, y, xe - 1, y, color);
        gfx_line(x, ye - 1, xe - 1, ye - 1, color);
        gfx_line(x, y, x, ye - 1, color);
        gfx_line(xe - 1, y, xe - 1, ye - 1, color);
        return;
    }
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, xe), y1 = MIN(H, ye);
    volatile uint8_t* px = classic_px();
    for (int yy = y0; yy < y1; yy++)
        if (x1 > x0) memset((void*)(px + yy * W + x0), color, (size_t)(x1 - x0));
    present(x0, y0, x1, y1);
}

void gfx_text(int x, int y, const char* s, uint8_t color) {
    int x0 = x;
    for (; *s && x < W; s++, x += 9) {
        const uint8_t* g = &font8x16[(uint8_t)*s * 16];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) {
                    int px = x + col, py = y + row;
                    if (px >= 0 && px < W && py >= 0 && py < H) classic_px()[py * W + px] = color;
                }
    }
    present(x0, y, x + 8, y + 16);
}

// ---- Native drawing ----

// A w x h rectangle of 0xRRGGBB pixels, `stride` pixels from one row
// to the next, to x, y on the screen; what falls off the screen is
// dropped. pixels is the program's memory, which the caller has checked
// the whole of.
void gfx_put(int x, int y, int w, int h, const uint32_t* pixels, int stride) {
    int sw = on_fb ? (int)fb_w : W, sh = on_fb ? (int)fb_h : H;
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(sw, end_of(x, w)), y1 = MIN(sh, end_of(y, h));
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) {
        const uint32_t* src = pixels + (size_t)(yy - y) * (size_t)stride + (x0 - x);
        if (on_fb) {
            volatile uint8_t* p = fb + (uint32_t)yy * fb_pitch + (uint32_t)x0 * fb_bytes;
            if (fb_bytes == 4 && boot_info.fb.red_pos == 16 && boot_info.fb.green_pos == 8 && boot_info.fb.blue_pos == 0) {
                volatile uint32_t* q = (volatile uint32_t*)p;
                for (int i = 0; i < x1 - x0; i++) q[i] = src[i] & 0xFFFFFF;
            } else {
                for (int i = 0; i < x1 - x0; i++, p += fb_bytes) put_px(p, pixel_rgb(src[i]));
            }
        } else {
            volatile uint8_t* p = VGA_FB + yy * W + x0;
            for (int i = 0; i < x1 - x0; i++) {
                uint32_t c = src[i];
                p[i] = (uint8_t)((c >> 16 & 0xE0) | (c >> 11 & 0x1C) | (c >> 6 & 0x03));
            }
        }
    }
}

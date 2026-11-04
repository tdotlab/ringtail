// VGA graphics: mode 13h (320x200, 256 colors), the mouse cursor,
// and the graphics demo. Plain terms first: text mode shows
// characters from a grid, graphics mode shows individual pixels
// from a framebuffer, a block of memory where one byte is one
// pixel's color. In mode 13h that block lives at 0xA0000, is
// 320*200 = 64000 bytes, and pixel (x, y) is byte y*320 + x.
// That address is below 16 MB, so it is already identity-mapped
// by paging, and the kernel can write it directly, no extra
// mapping needed.
//
// Why the mode switch is done by writing VGA registers, not by
// a BIOS call: the kernel runs in protected mode, where BIOS
// interrupts do not work (they need real mode). The VGA chip is
// programmed directly through its ports, with the standard mode
// 13h register tables: miscellaneous output, sequencer, CRTC
// (the timing generator), graphics controller, and attribute
// controller. The tables below are the published mode 13h values;
// QEMU's screendump is the verification that they took effect.
//
// Save and restore, the whole point of doing this as a demo
// rather than a one-way trip: before switching, the driver saves
// the VGA register state, the color palette, the text screen
// (4000 bytes at 0xB8000), and the text font. After the demo it
// restores all four, so the shell comes back exactly as it was.
// The font needs special care: mode 13h writes go through the
// same video memory the font lives in (plane 2), so the demo
// would clobber the font if it were not saved and restored too.
// The font itself is not embedded here: it is read out of video
// memory plane 2, where the BIOS loaded it at boot, so the demo
// text uses the real VGA font, not a hand-drawn approximation.
//
// The cursor is drawn with save-underneath and restore: before
// drawing the arrow, the pixels under it are copied to RAM, and
// when the cursor moves, those pixels are put back before the
// arrow is drawn at the new spot. That is what keeps the cursor
// from leaving a trail, and what keeps a dot drawn by a click
// intact when the cursor passes over it.

#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define FB_WIDTH 320
#define FB_HEIGHT 200
static volatile uint8_t* const FB = (volatile uint8_t*)0xA0000;
static volatile uint8_t* const TEXT_BUF = (volatile uint8_t*)0xB8000;

// ---- VGA register tables for mode 13h ----
// Sequencer (port 0x3C4 index, 0x3C5 data), indices 0-4.
static const uint8_t seq_13h[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
// CRTC (0x3D4/0x3D5), indices 0-24.
static const uint8_t crtc_13h[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28,
    0x40, 0x96, 0xB9, 0xA3, 0xFF
};
// Graphics controller (0x3CE/0x3CF), indices 0-8.
static const uint8_t gfx_13h[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};
// Attribute controller (0x3C0), indices 0-20. Palette entries
// 0-15 are identity (color n shows as palette n), then mode
// control 0x41 (graphics mode, palette enabled), overscan 0,
// plane enable all, panning 0, color select 0.
static const uint8_t attr_13h[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};
#define MISC_13H 0x63

// Saved text-mode state, filled by gfx_save_state before the
// switch and written back by gfx_restore_state after it.
static uint8_t save_misc;
static uint8_t save_seq[5];
static uint8_t save_crtc[25];
static uint8_t save_gfx[9];
static uint8_t save_attr[21];
static uint8_t save_palette[256 * 3];
static uint8_t save_text[80 * 25 * 2];
static uint8_t save_font[256 * 32];

// The working font: 256 glyphs, 16 rows each, copied from video
// memory plane 2 at demo start. Glyph n is font[n*32 + row]:
// the VGA character generator addresses each glyph in a 32-byte
// slot (char*32 + row), so an 8x16 font uses the first 16 bytes
// of each slot and the rest is padding. Reading with a 16-byte
// stride, as the first version did, maps every character to the
// wrong glyph, which is exactly what the first screenshots showed.
// Bit 7 is the leftmost pixel of a row.
static uint8_t font_buf[256 * 32];

// ---- Register access helpers ----
// Attribute controller: every access goes through port 0x3C0,
// which alternates between "index" and "data" on each write, so
// the flip-flop must be reset (by reading 0x3DA) before an index
// write, or an index can be mistaken for data. Resetting before
// every register, rather than once per table, costs one port
// read and removes all doubt about the toggle state.
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
// After writing attribute registers, palette access must be
// re-enabled: reset the flip-flop and write 0x20, which is index
// 0 with the palette-enable bit set. Without this the screen
// stays blank, because the palette is switched off.
static void ac_enable_palette(void) {
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);
}

static void write_seq(const uint8_t* t) {
    // Synchronous reset on, write regs 1-4, reset off: the
    // sequencer must be held in reset while its clocking and
    // memory-mode registers change, or the chip can glitch.
    outb(0x3C4, 0); outb(0x3C5, 0x01);
    for (int i = 1; i < 5; i++) { outb(0x3C4, (uint8_t)i); outb(0x3C5, t[i]); }
    outb(0x3C4, 0); outb(0x3C5, t[0]);
}
static void write_crtc(const uint8_t* t) {
    // CRTC registers 0-7 are write-protected while register 0x11
    // bit 7 is set, so clear it first; the table itself then sets
    // 0x11 to its real value (0x0E here, protect bit clear).
    outb(0x3D4, 0x11); outb(0x3D5, 0x00);
    for (int i = 0; i < 25; i++) { outb(0x3D4, (uint8_t)i); outb(0x3D5, t[i]); }
}
static void write_gfx(const uint8_t* t) {
    for (int i = 0; i < 9; i++) { outb(0x3CE, (uint8_t)i); outb(0x3CF, t[i]); }
}
static void write_attr(const uint8_t* t) {
    for (int i = 0; i < 21; i++) ac_write((uint8_t)i, t[i]);
    ac_enable_palette();
}

static void gfx_save_state(void) {
    save_misc = inb(0x3CC);
    for (int i = 0; i < 5; i++) { outb(0x3C4, (uint8_t)i); save_seq[i] = inb(0x3C5); }
    for (int i = 0; i < 25; i++) { outb(0x3D4, (uint8_t)i); save_crtc[i] = inb(0x3D5); }
    for (int i = 0; i < 9; i++) { outb(0x3CE, (uint8_t)i); save_gfx[i] = inb(0x3CF); }
    for (int i = 0; i < 21; i++) save_attr[i] = ac_read((uint8_t)i);
    for (int i = 0; i < 256; i++) {
        outb(0x3C7, (uint8_t)i);
        save_palette[i * 3] = inb(0x3C9);
        save_palette[i * 3 + 1] = inb(0x3C9);
        save_palette[i * 3 + 2] = inb(0x3C9);
    }
    for (int i = 0; i < 80 * 25 * 2; i++) save_text[i] = TEXT_BUF[i];
}

static void gfx_restore_state(void) {
    outb(0x3C2, save_misc);
    write_seq(save_seq);
    write_crtc(save_crtc);
    write_gfx(save_gfx);
    write_attr(save_attr);
    outb(0x3C8, 0);
    for (int i = 0; i < 256 * 3; i++) outb(0x3C9, save_palette[i]);
    for (int i = 0; i < 80 * 25 * 2; i++) TEXT_BUF[i] = save_text[i];
}

// ---- Font in and out of video memory plane 2 ----
// Reading or writing a single plane needs two temporary changes,
// both restored afterwards: the graphics mode register must have
// odd/even addressing off (text mode has it on, and it overrides
// the plane selection with the address bit), and the memory map
// must point at 0xA0000 so plane memory is CPU-visible. Display
// is unaffected: it reads video memory through the CRTC, not
// through these CPU-access registers.
static void plane_access_begin(uint8_t plane, uint8_t* save_mode, uint8_t* save_misc2, uint8_t* save_map) {
    outb(0x3CE, 5); *save_mode = inb(0x3CF);
    outb(0x3C4, 2); *save_map = inb(0x3C5);
    outb(0x3CE, 6); *save_misc2 = inb(0x3CF);
    outb(0x3CE, 5); outb(0x3CF, 0x00); // write mode 0, odd/even off
    // Sequencer map mask is a BITMASK, not a plane number: bit n
    // enables plane n. Plane 2 alone is 0x04, not 0x02. Writing
    // 0x02 here selected plane 1, so the first font restore
    // overwrote the text attributes with font bytes (the rainbow
    // garbage after the demo) and left the real font broken.
    // The graphics read map above IS a plane number, so reads
    // were correct all along, which is why demo text worked.
    outb(0x3C4, 2); outb(0x3C5, (uint8_t)(1 << plane));
    outb(0x3CE, 4); outb(0x3CF, plane); // graphics read map: this plane (reads)
    outb(0x3CE, 6); outb(0x3CF, (uint8_t)((*save_misc2 & ~0x0C) | 0x00)); // A0000, 128K
}
static void plane_access_end(uint8_t save_mode, uint8_t save_misc2, uint8_t save_map) {
    outb(0x3CE, 4); outb(0x3CF, 0x00);
    outb(0x3C4, 2); outb(0x3C5, save_map);
    outb(0x3CE, 5); outb(0x3CF, save_mode);
    outb(0x3CE, 6); outb(0x3CF, save_misc2);
}

static void gfx_load_font(void) {
    uint8_t sm, smisc, smap;
    plane_access_begin(2, &sm, &smisc, &smap);
    for (int i = 0; i < 256 * 32; i++) font_buf[i] = FB[i];
    plane_access_end(sm, smisc, smap);
    for (int i = 0; i < 256 * 32; i++) save_font[i] = font_buf[i];
}
static void gfx_restore_font(void) {
    uint8_t sm, smisc, smap;
    plane_access_begin(2, &sm, &smisc, &smap);
    for (int i = 0; i < 256 * 32; i++) FB[i] = save_font[i];
    plane_access_end(sm, smisc, smap);
}

// ---- Palette ----
// DAC values are 6-bit (0-63), written as R, G, B triples after
// selecting an index on 0x3C8; the index auto-increments.
static void gfx_set_palette(const uint8_t* rgb, int count) {
    outb(0x3C8, 0);
    for (int i = 0; i < count * 3; i++) outb(0x3C9, rgb[i]);
}
static const uint8_t demo_palette[16 * 3] = {
    0, 0, 0,     0, 0, 42,    0, 42, 0,    0, 42, 42,
    42, 0, 0,    42, 0, 42,   42, 21, 0,   42, 42, 42,
    21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,
    63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63
};

// ---- Drawing primitives ----
// Every primitive goes through gfx_pixel, so the bounds check
// here is the only clipping the driver needs: anything drawn
// partly off screen is cut cleanly instead of writing past the
// 64K framebuffer into unrelated video memory.
void gfx_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    FB[y * FB_WIDTH + x] = color;
}
void gfx_clear(uint8_t color) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) FB[i] = color;
}
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color) {
    // Bresenham: walk the longer axis, keep a running error for
    // the shorter one, step it when the error crosses half.
    int dx = x1 - x0, dy = y1 - y0;
    int sx = (dx > 0) ? 1 : -1, sy = (dy > 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    while (1) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}
void gfx_rect(int x, int y, int w, int h, uint8_t color, int filled) {
    if (filled) {
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
                gfx_pixel(x + xx, y + yy, color);
    } else {
        gfx_line(x, y, x + w - 1, y, color);
        gfx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
        gfx_line(x, y, x, y + h - 1, color);
        gfx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
    }
}
void gfx_circle(int cx, int cy, int r, uint8_t color) {
    // Midpoint circle: eight symmetric points per step, walking
    // the first octant and mirroring it around the center.
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        gfx_pixel(cx + x, cy + y, color); gfx_pixel(cx - x, cy + y, color);
        gfx_pixel(cx + x, cy - y, color); gfx_pixel(cx - x, cy - y, color);
        gfx_pixel(cx + y, cy + x, color); gfx_pixel(cx - y, cy + x, color);
        gfx_pixel(cx + y, cy - x, color); gfx_pixel(cx - y, cy - x, color);
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}
void gfx_text(int x, int y, const char* s, uint8_t color) {
    int cx = x;
    while (*s) {
        const uint8_t* g = &font_buf[(uint8_t)*s * 32];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) gfx_pixel(cx + col, y + row, color);
            }
        }
        cx += 9; // 8 pixel glyph + 1 pixel spacing
        s++;
    }
}

// ---- Cursor ----
// Arrow, 12 wide by 16 tall, tip at the top-left (the hotspot:
// the reported mouse position is the tip, so a click draws
// exactly where the tip points). Rows are 12-bit values, bit 11
// is the leftmost pixel.
#define CUR_W 12
#define CUR_H 16
static const uint16_t cursor_shape[CUR_H] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xF80, 0xEC0, 0xC60, 0x830, 0x018, 0x000, 0x000, 0x000
};
static uint8_t cursor_saved[CUR_W * CUR_H];
static int cursor_x = -1, cursor_y = -1, cursor_on = 0;

static void cursor_show(int x, int y) {
    for (int row = 0; row < CUR_H; row++) {
        for (int col = 0; col < CUR_W; col++) {
            int sx = x + col, sy = y + row;
            if (sx >= 0 && sx < FB_WIDTH && sy >= 0 && sy < FB_HEIGHT)
                cursor_saved[row * CUR_W + col] = FB[sy * FB_WIDTH + sx];
        }
    }
    for (int row = 0; row < CUR_H; row++) {
        for (int col = 0; col < CUR_W; col++) {
            if (cursor_shape[row] & (0x800 >> col))
                gfx_pixel(x + col, y + row, 15);
        }
    }
    cursor_x = x; cursor_y = y; cursor_on = 1;
}
static void cursor_hide(void) {
    if (!cursor_on) return;
    for (int row = 0; row < CUR_H; row++) {
        for (int col = 0; col < CUR_W; col++) {
            int sx = cursor_x + col, sy = cursor_y + row;
            if (sx >= 0 && sx < FB_WIDTH && sy >= 0 && sy < FB_HEIGHT)
                FB[sy * FB_WIDTH + sx] = cursor_saved[row * CUR_W + col];
        }
    }
    cursor_on = 0;
}

// ---- Graphics mode entry/exit, shared by the kernel demo and
// by user programs through the graphics syscalls ----
extern void text_cursor_get(int* x, int* y);
extern void text_cursor_set(int x, int y);
static int saved_cx = 0, saved_cy = 0;
// gfx_graphics_begin: save text state, load the font (which must
// happen while still in text mode, because the font lives in
// video memory plane 2), switch to mode 13h, set the demo
// palette, clear to blue. gfx_graphics_end undoes it, in the
// reverse order the restore needs: registers and text screen
// first, then the font (which writes to plane 2 through the
// temporary plane-access settings, and must not run before the
// text registers are back, or its writes would land wrong).
void gfx_graphics_begin(void) {
    text_cursor_get(&saved_cx, &saved_cy);
    gfx_save_state();
    gfx_load_font();
    outb(0x3C2, MISC_13H);
    write_seq(seq_13h);
    write_crtc(crtc_13h);
    write_gfx(gfx_13h);
    write_attr(attr_13h);
    gfx_set_palette(demo_palette, 16);
    gfx_clear(1);
}
void gfx_graphics_end(void) {
    gfx_restore_state();
    gfx_restore_font();
    text_cursor_set(saved_cx, saved_cy);
}
extern int mouse_get_x(void);
extern int mouse_get_y(void);
extern int mouse_get_buttons(void);
extern int keyboard_poll(void);
extern uint32_t timer_get_ticks(void);

static void coords_text(char* buf, int x, int y, int b) {
    // Fixed-width, so the erase rectangle never needs resizing:
    // "X=160 Y=100 LMR", digits space-padded, buttons as letters
    // or dashes. Built by hand, no sprintf in a freestanding kernel.
    buf[0] = 'X'; buf[1] = '=';
    buf[2] = (x >= 100) ? (char)('0' + x / 100) : ' ';
    buf[3] = (x >= 10) ? (char)('0' + (x / 10) % 10) : ' ';
    buf[4] = (char)('0' + x % 10);
    buf[5] = ' ';
    buf[6] = 'Y'; buf[7] = '=';
    buf[8] = (y >= 100) ? (char)('0' + y / 100) : ' ';
    buf[9] = (y >= 10) ? (char)('0' + (y / 10) % 10) : ' ';
    buf[10] = (char)('0' + y % 10);
    buf[11] = ' ';
    buf[12] = (b & 1) ? 'L' : '-';
    buf[13] = (b & 4) ? 'M' : '-';
    buf[14] = (b & 2) ? 'R' : '-';
    buf[15] = 0;
}

void gfx_demo(void) {
    char coords[32];
    gfx_graphics_begin();

    gfx_text(10, 8, "RINGTAIL GRAPHICS", 15);
    gfx_rect(20, 35, 70, 40, 4, 1);
    gfx_rect(110, 35, 70, 40, 14, 0);
    gfx_line(20, 90, 300, 90, 10);
    gfx_circle(230, 55, 25, 12);
    gfx_text(10, 105, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 15);
    gfx_text(10, 125, "0123456789", 15);
    gfx_text(10, 150, "MOVE MOUSE - CLICK DRAWS", 11);

    int last_x = mouse_get_x();
    int last_y = mouse_get_y();
    int last_b = mouse_get_buttons();
    coords_text(coords, last_x, last_y, last_b);
    gfx_text(10, 170, coords, 15);
    cursor_show(last_x, last_y);

    // Frame loop, paced by the timer tick (~100 Hz): on each
    // frame, check the mouse and the keyboard queue. The keyboard
    // queue is normally drained by the main loop, but the main
    // loop is inside this demo (it is a shell command), so the
    // demo drains it itself, and any key press ends the demo.
    // hlt between frames idles the CPU until the next interrupt.
    uint32_t last_tick = timer_get_ticks();
    int exit_demo = 0, ext = 0;
    while (!exit_demo) {
        int x = mouse_get_x();
        int y = mouse_get_y();
        int b = mouse_get_buttons();
        if (x != last_x || y != last_y || b != last_b) {
            int left_edge = (b & 1) && !(last_b & 1);
            cursor_hide();
            // Coords redraw on ANY change, not just buttons: the
            // first version redrew only when buttons changed, so
            // a pure move left the cursor in the new spot with
            // the old coordinates underneath it.
            gfx_rect(10, 170, 200, 16, 1, 1);
            coords_text(coords, x, y, b);
            gfx_text(10, 170, coords, 15);
            if (left_edge) gfx_rect(x, y, 4, 4, 14, 1);
            cursor_show(x, y);
            last_x = x; last_y = y; last_b = b;
        }
        int sc;
        while ((sc = keyboard_poll()) >= 0) {
            if (ext) {
                ext = 0;
                if (!(sc & 0x80)) exit_demo = 1;
            } else if (sc == 0xE0) {
                ext = 1;
            } else if (!(sc & 0x80)) {
                exit_demo = 1; // a make code: a real key press
            }
            // Release codes are ignored: they are the key coming
            // back up, not a new press.
        }
        uint32_t now = timer_get_ticks();
        if (now == last_tick) {
            __asm__ volatile ("hlt");
        } else {
            last_tick = now;
        }
    }
    // Drain whatever is left in the queue (the exit key's release
    // code, a held shift) so nothing leaks into the shell line
    // after the demo returns.
    while (keyboard_poll() >= 0) { }
    cursor_hide();
    gfx_graphics_end();
}

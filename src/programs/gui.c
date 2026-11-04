// gui: RingTail's first graphical desktop. It is a user program,
// not kernel code: it draws only through the graphics syscalls
// (pixels, rectangles, lines, text), reads the mouse through
// MOUSE_STATE, and reads keys through KEY_GET, and builds its
// own windows, buttons, and icons out of those primitives. It is
// run like any other program, with "run gui", and it owns the
// display while it runs: GFX_BEGIN switches to graphics mode and
// saves the text screen, GFX_END restores it, so exiting the
// desktop (Quit button or Esc) returns to the shell exactly as
// it was, and a faulted GUI is restored by the kernel's cleanup
// hook, not left hanging in graphics mode.
// Design, kept deliberately small. There are two windows,
// Welcome and About, and two desktop icons that open them.
// Windows drag by their title bars, close with the X, and come
// to the front when clicked. The Welcome window has a button
// that counts clicks. There is no damage tracking: any change
// redraws the whole screen (desktop, bar, icons, windows in
// z-order, cursor last), which is simple, correct, and fast
// enough at 320x200, because a full redraw is a few hundred
// syscalls and only happens when something actually changed.
// The loop polls the mouse and keys: there is no blocking
// input wait yet, so the GUI uses its time slices spinning,
// which preemption makes harmless but wasteful, a known limit.
// Colors are palette indices, the kernel demo's palette.
#include "syscall_stub.h"

#define SCR_W 320
#define SCR_H 200
#define BAR_H 20
#define QUIT_X 270
#define QUIT_W 45

// Window slots: 0 = Welcome, 1 = About. zorder is back to front;
// with two windows, bringing one to the front is a swap.
typedef struct {
    int x, y, w, h, open, clicks;
    const char* title;
} window_t;

static window_t wins[2];
static int zorder[2] = {0, 1};
static int dragging = -1;
static int drag_dx = 0, drag_dy = 0;

// Cursor arrow, 12x16, same shape as the kernel demo's: rows are
// 12-bit values, bit 11 is the leftmost pixel, tip top-left.
static const uint16_t cursor_shape[16] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xF80, 0xEC0, 0xC60, 0x830, 0x018, 0x000, 0x000, 0x000
};

static void draw_rect(int x, int y, int w, int h, int color, int filled) {
    gfx_rect_t r;
    r.x = x; r.y = y; r.w = w; r.h = h; r.color = color; r.filled = filled;
    sys_gfx_rect(&r);
}
static void draw_text(int x, int y, const char* s, int color) {
    gfx_text_t t;
    t.x = x; t.y = y; t.str = s; t.color = color;
    sys_gfx_text(&t);
}
static int in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}
// Small number formatting: append a decimal number to a buffer,
// returns the new end. Programs carry this themselves; the kernel
// provides no formatting, by design.
static int append_num(char* buf, int n) {
    char tmp[12];
    int i = 0, j = 0;
    if (n == 0) { buf[0] = '0'; return 1; }
    while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) buf[j++] = tmp[--i];
    return j;
}
static void format_coords(char* buf, int x, int y) {
    int i = 0;
    buf[i++] = 'X'; buf[i++] = '=';
    i += append_num(buf + i, x);
    buf[i++] = ' '; buf[i++] = 'Y'; buf[i++] = '=';
    i += append_num(buf + i, y);
    buf[i] = 0;
}

static void draw_cursor(int mx, int my) {
    // Black shadow offset by (1,1), then white on top, in one
    // row-major pass: a shadow pixel is always drawn before the
    // white pixel it sits under, so white wins where they
    // overlap, and black peeks out on the right and bottom edges.
    for (int row = 0; row < 16; row++) {
        uint16_t bits = cursor_shape[row];
        for (int col = 0; col < 12; col++) {
            if (bits & (0x800 >> col)) {
                sys_gfx_pixel(mx + col + 1, my + row + 1, 0);
                sys_gfx_pixel(mx + col, my + row, 15);
            }
        }
    }
}

static void draw_window(int idx) {
    window_t* w = &wins[idx];
    // Body first, then title bar over its top, so the bar's
    // bottom edge is crisp and the body never overdraws it.
    draw_rect(w->x, w->y + BAR_H, w->w, w->h - BAR_H, 7, 1);
    draw_rect(w->x, w->y, w->w, BAR_H, 9, 1);
    draw_text(w->x + 5, w->y + 2, w->title, 15);
    draw_rect(w->x + w->w - 20, w->y + 2, 18, 16, 4, 1);
    draw_text(w->x + w->w - 14, w->y + 2, "X", 15);
    if (idx == 0) {
        // Welcome: two lines, a button, and its click count.
        draw_text(w->x + 10, w->y + 30, "Welcome to RingTail!", 0);
        draw_text(w->x + 10, w->y + 50, "Click the button:", 0);
        draw_rect(w->x + 10, w->y + 70, 90, 20, 1, 1);
        draw_text(w->x + 19, w->y + 72, "Click me", 15);
        char buf[32];
        int i = 0;
        const char* p = "Clicked: ";
        while (*p) buf[i++] = *p++;
        i += append_num(buf + i, w->clicks);
        buf[i] = 0;
        draw_text(w->x + 10, w->y + 95, buf, 0);
    } else {
        draw_text(w->x + 10, w->y + 30, "RingTail v0.9.0", 0);
        draw_text(w->x + 10, w->y + 50, "Bare-metal x86 OS", 0);
        draw_text(w->x + 10, w->y + 70, "C + NASM, no libc", 0);
    }
}

static void draw_all(mouse_state_t* ms) {
    char coords[32];
    sys_gfx_clear(1);
    // Top bar, with title, live mouse coordinates, and Quit.
    draw_rect(0, 0, SCR_W, BAR_H, 7, 1);
    draw_text(5, 2, "RingTail", 0);
    format_coords(coords, ms->x, ms->y);
    draw_text(120, 2, coords, 0);
    draw_rect(QUIT_X, 2, QUIT_W, 16, 4, 1);
    draw_text(QUIT_X + 4, 2, "Quit", 15);
    // Desktop icons: a filled square and a label under it.
    draw_rect(10, 30, 50, 35, 14, 1);
    draw_text(10, 68, "Welcome", 15);
    draw_rect(10, 90, 50, 35, 11, 1);
    draw_text(10, 128, "About", 15);
    // Windows back to front, so the front window overdraws.
    for (int zi = 0; zi < 2; zi++) {
        if (wins[zorder[zi]].open) draw_window(zorder[zi]);
    }
    draw_cursor(ms->x, ms->y);
}

static void bring_to_front(int idx) {
    if (zorder[1] != idx) {
        zorder[1] = idx;
        zorder[0] = 1 - idx;
    }
}

__attribute__((section(".text._start"))) void _start(void) {
    if (sys_gfx_begin() != 0) {
        const char* msg = "Graphics busy: another program owns it\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    wins[0].x = 80; wins[0].y = 50; wins[0].w = 180; wins[0].h = 120;
    wins[0].open = 0; wins[0].clicks = 0; wins[0].title = "Welcome";
    wins[1].x = 100; wins[1].y = 70; wins[1].w = 170; wins[1].h = 100;
    wins[1].open = 0; wins[1].clicks = 0; wins[1].title = "About";

    mouse_state_t ms, prev;
    sys_mouse_state(&ms);
    prev = ms;
    draw_all(&ms);
    int quit = 0;
    while (!quit) {
        int need_redraw = 0;
        // Drain the whole key queue each frame: Esc (make code
        // 0x01, with the 0xE0 prefix skipped) quits, everything
        // else is discarded, so the queue cannot grow and Esc
        // stays responsive even if keys were held during the GUI.
        int sc, e0 = 0;
        while ((sc = sys_key_get()) >= 0) {
            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x01 && !e0) quit = 1;
            e0 = 0;
        }
        sys_mouse_state(&ms);
        int left_down = (ms.buttons & 1) && !(prev.buttons & 1);
        int left_up = !(ms.buttons & 1) && (prev.buttons & 1);
        if (ms.x != prev.x || ms.y != prev.y) need_redraw = 1;
        if (left_up) dragging = -1;
        if (dragging >= 0 && (ms.x != prev.x || ms.y != prev.y)) {
            window_t* w = &wins[dragging];
            w->x = ms.x - drag_dx;
            w->y = ms.y - drag_dy;
            if (w->x < 0) w->x = 0;
            if (w->x > SCR_W - w->w) w->x = SCR_W - w->w;
            if (w->y < BAR_H) w->y = BAR_H;
            if (w->y > SCR_H - w->h) w->y = SCR_H - w->h;
            need_redraw = 1;
        }
        if (left_down && dragging < 0) {
            int handled = 0;
            if (in_rect(ms.x, ms.y, QUIT_X, 2, QUIT_W, 16)) {
                quit = 1;
                handled = 1;
            }
            // Windows front to back: the front window gets the
            // click first, and a click anywhere on it brings it
            // to the front, even if it was behind.
            for (int zi = 1; zi >= 0 && !handled; zi--) {
                int idx = zorder[zi];
                window_t* w = &wins[idx];
                if (!w->open) continue;
                if (in_rect(ms.x, ms.y, w->x + w->w - 20, w->y + 2, 18, 16)) {
                    w->open = 0;
                    need_redraw = 1;
                    handled = 1;
                } else if (in_rect(ms.x, ms.y, w->x, w->y, w->w, BAR_H)) {
                    bring_to_front(idx);
                    dragging = idx;
                    drag_dx = ms.x - w->x;
                    drag_dy = ms.y - w->y;
                    need_redraw = 1;
                    handled = 1;
                } else if (idx == 0 && in_rect(ms.x, ms.y, w->x + 10, w->y + 70, 90, 20)) {
                    bring_to_front(idx);
                    w->clicks++;
                    need_redraw = 1;
                    handled = 1;
                } else if (in_rect(ms.x, ms.y, w->x, w->y + BAR_H, w->w, w->h - BAR_H)) {
                    bring_to_front(idx);
                    need_redraw = 1;
                    handled = 1;
                }
            }
            if (!handled) {
                if (in_rect(ms.x, ms.y, 10, 30, 63, 55)) {
                    wins[0].open = 1;
                    bring_to_front(0);
                    need_redraw = 1;
                } else if (in_rect(ms.x, ms.y, 10, 90, 50, 55)) {
                    wins[1].open = 1;
                    bring_to_front(1);
                    need_redraw = 1;
                }
            }
        }
        if (need_redraw) draw_all(&ms);
        prev = ms;
    }
    sys_gfx_end();
    sys_exit();
}

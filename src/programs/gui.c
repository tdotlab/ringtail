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
// v0.10.2 adds the launcher: at startup the GUI lists every
// file on disk through LIST, keeps the ones with no dot in
// the name (programs have no extension, data files do), and
// draws one green icon per program, up to nine, in columns
// to the right of the built-in icons. Clicking a program
// icon exits graphics first, then spawns the program
// through SPAWN, prints the result the way the shell does,
// and exits, so the new program runs in text mode, where
// its output is visible. That order is required, not just
// polite: SPAWN refuses while the caller owns graphics,
// because a program started inside graphics mode would
// print to a text screen nobody can see, and the restore
// on graphics exit would overwrite it. LIST is safe in
// graphics mode, because it returns data to the program,
// not output to the screen, so the startup list happens
// after GFX_BEGIN without any special handling.
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

// Launcher programs, from LIST at startup. Nine is the
// desktop's capacity: three columns of three, to the right
// of the built-in icons, and any programs past nine are
// simply not shown, not paged or scrolled, a known limit.
// Names are display names (uppercase, 8.3, no dot, because
// files with a dot are data files and are filtered out).
#define MAX_PROGS 9
static char prog_names[MAX_PROGS][13];
static int prog_count = 0;

// Program icon position for index i: column i/3, row i%3.
// Columns start at x=80, so they never overlap the
// built-in icons (the Welcome click area ends at x=73),
// and rows match the built-in rows (y=26, 86, 146).
// Rows start at 26, not 30, so the bottom row's label
// ends exactly at the screen's bottom edge (146 + 38
// for the label offset + 16 for the glyph height = 200):
// at y=30 the bottom labels were clipped by 4 pixels.
static void prog_xy(int i, int* x, int* y) {
    *x = 80 + (i / 3) * 80;
    *y = 26 + (i % 3) * 60;
}

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
        draw_text(w->x + 10, w->y + 30, "RingTail v0.10.2", 0);
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
    // Built-in icons first (yellow Welcome, cyan About),
    // then green program icons from LIST, same size and
    // same label offset (label 38 below the icon top, so
    // it sits 3 below the icon's bottom edge).
    draw_rect(10, 26, 50, 35, 14, 1);
    draw_text(10, 64, "Welcome", 15);
    draw_rect(10, 86, 50, 35, 11, 1);
    draw_text(10, 124, "About", 15);
    for (int i = 0; i < prog_count; i++) {
        int px, py;
        prog_xy(i, &px, &py);
        draw_rect(px, py, 50, 35, 10, 1);
        draw_text(px, py + 38, prog_names[i], 15);
    }
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
    // Launcher list: LIST every file, keep only names with
    // no dot (programs), up to nine. A LIST failure at
    // index 0 (no disk, or an empty disk) just means no
    // program icons, not an error: the GUI still works
    // with its built-in windows.
    {
        char namebuf[16];
        for (int idx = 0; prog_count < MAX_PROGS; idx++) {
            if (sys_list(idx, namebuf, 16) != 0) break;
            int has_dot = 0;
            for (int j = 0; namebuf[j]; j++) if (namebuf[j] == '.') has_dot = 1;
            if (has_dot) continue;
            int j = 0;
            while (namebuf[j]) { prog_names[prog_count][j] = namebuf[j]; j++; }
            prog_names[prog_count][j] = 0;
            prog_count++;
        }
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
                if (in_rect(ms.x, ms.y, 10, 26, 63, 55)) {
                    wins[0].open = 1;
                    bring_to_front(0);
                    need_redraw = 1;
                } else if (in_rect(ms.x, ms.y, 10, 86, 50, 55)) {
                    wins[1].open = 1;
                    bring_to_front(1);
                    need_redraw = 1;
                } else {
                    // Program icons, checked after windows
                    // and built-in icons, so a click on an
                    // open window never launches through it.
                    // The click area is the icon plus its
                    // label: width is the wider of the icon
                    // (50) and the label (9 per character),
                    // height 55 covers icon plus label.
                    for (int i = 0; i < prog_count; i++) {
                        int px, py;
                        prog_xy(i, &px, &py);
                        int lw = prog_strlen(prog_names[i]) * 9;
                        int cw = lw > 50 ? lw : 50;
                        if (!in_rect(ms.x, ms.y, px, py, cw, 55)) continue;
                        // Launch, and exit directly, not
                        // through quit: graphics is already
                        // ended, so the loop-end GFX_END
                        // must not run a second time.
                        sys_gfx_end();
                        int id = sys_spawn(prog_names[i]);
                        if (id >= 0) {
                            sys_print("Started ", 8);
                            sys_print(prog_names[i], prog_strlen(prog_names[i]));
                            sys_print(" as task ", 9);
                            char numbuf[12];
                            int nl = append_num(numbuf, id);
                            sys_print(numbuf, nl);
                            sys_print("\n", 1);
                        } else {
                            sys_print("spawn failed\n", 13);
                        }
                        sys_exit();
                    }
                }
            }
        }
        if (need_redraw) draw_all(&ms);
        prev = ms;
    }
    sys_gfx_end();
    sys_exit();
}

// A small desktop: draggable windows, a program launcher, and a
// mouse cursor. Frames are drawn into a buffer in this program's own
// memory and sent to the screen in one call, so nothing flickers.
// Esc or the Quit button leaves.
#include "ulib.h"

#define W GFX_WIDTH
#define H GFX_HEIGHT
#define BAR_H 20
#define QUIT_X 270
#define QUIT_W 45
#define PREV_X 200
#define NEXT_X 240
#define PAGE_BTN_W 18
#define PER_PAGE 9
#define MAX_PROGS 64
#define SC_ESC 0x01

static uint8_t fb[W * H];
static uint8_t font[256 * 16];

// ---- Drawing into the frame buffer ----

static void px(int x, int y, int c) {
    if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = (uint8_t)c;
}

static void rect(int x, int y, int w, int h, int c) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    for (int yy = y0; yy < y1; yy++)
        if (x1 > x0) memset(fb + yy * W + x0, c, (size_t)(x1 - x0));
}

static int text_width(const char* s) { return (int)strlen(s) * 9; }

static void text(int x, int y, const char* s, int c) {
    for (; *s; s++, x += 9) {
        const uint8_t* g = &font[(uint8_t)*s * 16];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) px(x + col, y + row, c);
    }
}

static const uint16_t cursor_shape[16] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xF80, 0xEC0, 0xC60, 0x830, 0x018, 0x000, 0x000, 0x000
};

static void cursor(int x, int y) {
    // Shadow first, then white on top, so black shows only at the edges.
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 12; c++)
            if (cursor_shape[r] & (0x800 >> c)) px(x + c + 1, y + r + 1, 0);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 12; c++)
            if (cursor_shape[r] & (0x800 >> c)) px(x + c, y + r, 15);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// ---- Desktop state ----

typedef struct {
    int x, y, w, h, open, clicks;
    const char* title;
} window_t;

static window_t wins[2] = {
    { 80, 50, 180, 120, 0, 0, "Welcome" },
    { 100, 70, 170, 100, 0, 0, "About" },
};
static int zorder[2] = { 0, 1 };  // back to front
static int dragging = -1, drag_dx, drag_dy;

static char progs[MAX_PROGS][16];
static int prog_count, page;

static int pages(void) { return (prog_count + PER_PAGE - 1) / PER_PAGE; }

static void prog_xy(int slot, int* x, int* y) {
    *x = 80 + (slot / 3) * 80;
    *y = 26 + (slot % 3) * 60;
}

static void load_programs(void) {
    char name[16];
    for (int i = 0; prog_count < MAX_PROGS && sys_list(i, name, sizeof name) == 0; i++)
        if (!strchr(name, '.') && strcmp(name, "GUI") != 0) strlcpy(progs[prog_count++], name, 16);
}

static void draw_window(int idx) {
    window_t* w = &wins[idx];
    rect(w->x, w->y + BAR_H, w->w, w->h - BAR_H, 7);
    rect(w->x, w->y, w->w, BAR_H, 9);
    text(w->x + 5, w->y + 2, w->title, 15);
    rect(w->x + w->w - 20, w->y + 2, 18, 16, 4);
    text(w->x + w->w - 14, w->y + 2, "X", 15);
    if (idx == 0) {
        char buf[32];
        text(w->x + 10, w->y + 30, "Welcome to RingTail!", 0);
        text(w->x + 10, w->y + 50, "Click the button:", 0);
        rect(w->x + 10, w->y + 70, 90, 20, 1);
        text(w->x + 19, w->y + 72, "Click me", 15);
        snprintf(buf, sizeof buf, "Clicked: %d", w->clicks);
        text(w->x + 10, w->y + 95, buf, 0);
    } else {
        text(w->x + 10, w->y + 30, "RingTail v0.12", 0);
        text(w->x + 10, w->y + 50, "Bare-metal x86 OS", 0);
        text(w->x + 10, w->y + 70, "C + NASM, no libc", 0);
    }
}

static void draw(const mouse_state_t* m) {
    char buf[32];
    memset(fb, 1, sizeof fb);
    rect(0, 0, W, BAR_H, 7);
    text(5, 2, "RingTail", 0);
    snprintf(buf, sizeof buf, "%d,%d", m->x, m->y);
    text(100, 2, buf, 0);
    if (pages() > 1) {
        rect(PREV_X, 2, PAGE_BTN_W, 16, 8);
        text(PREV_X + 5, 2, "<", 15);
        snprintf(buf, sizeof buf, "%d", page + 1);
        text(PREV_X + PAGE_BTN_W + 5, 2, buf, 0);
        rect(NEXT_X, 2, PAGE_BTN_W, 16, 8);
        text(NEXT_X + 5, 2, ">", 15);
    }
    rect(QUIT_X, 2, QUIT_W, 16, 4);
    text(QUIT_X + 4, 2, "Quit", 15);

    rect(10, 26, 50, 35, 14);
    text(10, 64, "Welcome", 15);
    rect(10, 86, 50, 35, 11);
    text(10, 124, "About", 15);
    for (int s = 0; s < PER_PAGE && page * PER_PAGE + s < prog_count; s++) {
        int x, y;
        prog_xy(s, &x, &y);
        rect(x, y, 50, 35, 10);
        text(x, y + 38, progs[page * PER_PAGE + s], 15);
    }
    for (int z = 0; z < 2; z++)
        if (wins[zorder[z]].open) draw_window(zorder[z]);
    cursor(m->x, m->y);
    sys_gfx_blit(fb);
}

static void to_front(int idx) {
    zorder[1] = idx;
    zorder[0] = 1 - idx;
}

// Leave graphics, run a program in text mode, wait for it, and come
// back when the user presses Enter. Returns 0 to keep going.
static int launch(const char* name) {
    sys_gfx_end();
    int id = sys_spawn(name);
    if (id < 0) {
        printf("gui: cannot start %s: %s\n", name, strerror(id));
    } else {
        int status = sys_join(id);
        printf("[%s exited with status %d]\n", name, status);
    }
    print("Press Enter to return to the desktop.");
    char line[8];
    int r = sys_read(STDIN_FILENO, line, sizeof line);
    puts("");
    if (r < 0 && r != -EIO) return -1;
    return sys_gfx_begin();
}

// Returns 1 to quit.
static int click(int mx, int my) {
    if (inside(mx, my, QUIT_X, 2, QUIT_W, 16)) return 1;
    if (pages() > 1 && inside(mx, my, PREV_X, 2, PAGE_BTN_W, 16)) {
        page = (page + pages() - 1) % pages();
        return 0;
    }
    if (pages() > 1 && inside(mx, my, NEXT_X, 2, PAGE_BTN_W, 16)) {
        page = (page + 1) % pages();
        return 0;
    }
    // Windows front to back, so the front one gets the click.
    for (int z = 1; z >= 0; z--) {
        int idx = zorder[z];
        window_t* w = &wins[idx];
        if (!w->open || !inside(mx, my, w->x, w->y, w->w, w->h)) continue;
        if (inside(mx, my, w->x + w->w - 20, w->y + 2, 18, 16)) {
            w->open = 0;
            return 0;
        }
        to_front(idx);
        if (my < w->y + BAR_H) {
            dragging = idx;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        } else if (idx == 0 && inside(mx, my, w->x + 10, w->y + 70, 90, 20)) {
            w->clicks++;
        }
        return 0;
    }
    if (inside(mx, my, 10, 26, 63, 55)) { wins[0].open = 1; to_front(0); return 0; }
    if (inside(mx, my, 10, 86, 50, 55)) { wins[1].open = 1; to_front(1); return 0; }
    for (int s = 0; s < PER_PAGE && page * PER_PAGE + s < prog_count; s++) {
        int x, y;
        prog_xy(s, &x, &y);
        const char* name = progs[page * PER_PAGE + s];
        if (inside(mx, my, x, y, MAX(50, text_width(name)), 55))
            return launch(name) != 0;
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = sys_gfx_begin();
    if (r < 0) {
        printf("gui: cannot take the display: %s\n", strerror(r));
        return 1;
    }
    sys_gfx_font(font);
    load_programs();

    mouse_state_t m, prev;
    sys_mouse_state(&m);
    prev = m;
    draw(&m);
    for (int quit = 0; !quit;) {
        int sc, e0 = 0;
        while ((sc = sys_key_get()) >= 0) {
            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == SC_ESC && !e0) quit = 1;
            e0 = 0;
        }
        sys_mouse_state(&m);
        int down = (m.buttons & 1) && !(prev.buttons & 1);
        int up = !(m.buttons & 1) && (prev.buttons & 1);
        if (up) dragging = -1;
        if (dragging >= 0) {
            window_t* w = &wins[dragging];
            w->x = MAX(0, MIN(W - w->w, m.x - drag_dx));
            w->y = MAX(BAR_H, MIN(H - w->h, m.y - drag_dy));
        }
        if (down && dragging < 0 && click(m.x, m.y)) quit = 1;
        if (quit) break;
        draw(&m);
        prev = m;
        if (sys_wait(prev.x, prev.y, prev.buttons) < 0) break;
    }
    sys_gfx_end();
    return 0;
}

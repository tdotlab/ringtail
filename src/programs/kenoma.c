// Kenoma, the desktop: a program launcher, text files that open in
// Octavo, a draggable About window, and a mouse cursor. Frames are drawn into a buffer in this program's own
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
    int x, y, w, h, open;
} window_t;

static window_t about = { 100, 60, 170, 100, 0 };
static int dragging, drag_dx, drag_dy;

// Icons: text files first, then programs.
typedef struct {
    char name[NAME_MAX + 1];  // the file name on disk
    char label[9];            // under the icon: the name without its extension, cut to 8
    char ext[4];              // on a page: the extension's first three letters; empty for a program
} icon_t;

static icon_t progs[MAX_PROGS];
static int prog_count, page;

static int pages(void) { return (prog_count + PER_PAGE - 1) / PER_PAGE; }

static void prog_xy(int slot, int* x, int* y) {
    *x = 80 + (slot / 3) * 80;
    *y = 26 + (slot % 3) * 60;
}

// Text files Octavo opens from the desktop.
static int is_document(const char* ext) {
    return strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "lem") == 0 || strcasecmp(ext, "lemur") == 0;
}

static void add_icon(const char* name) {
    icon_t* ic = &progs[prog_count++];
    strlcpy(ic->name, name, sizeof ic->name);
    const char* dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    if (base >= sizeof ic->label) {  // a long name: seven letters and a ~
        memcpy(ic->label, name, sizeof ic->label - 2);
        ic->label[sizeof ic->label - 2] = '~';
        base = sizeof ic->label - 1;
    } else {
        memcpy(ic->label, name, base);
    }
    ic->label[base] = 0;
    strlcpy(ic->ext, dot ? dot + 1 : "", sizeof ic->ext);
    for (char* e = ic->ext; *e; e++)
        if (*e >= 'a' && *e <= 'z') *e = (char)(*e - 32);
}

static void load_programs(void) {
    char name[NAME_MAX + 1];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; prog_count < MAX_PROGS && sys_list(i, name, sizeof name) == 0; i++) {
            const char* dot = strrchr(name, '.');
            if (pass == 0 && dot && is_document(dot + 1)) add_icon(name);
            if (pass == 1 && !dot && strcasecmp(name, "kenoma") != 0) add_icon(name);
        }
    }
}

static void draw_icon(const icon_t* ic, int x, int y) {
    if (ic->ext[0]) {
        // A page with a folded corner, and the file type on it.
        rect(x + 8, y, 34, 35, 15);
        rect(x + 34, y, 8, 8, 1);
        rect(x + 34, y + 8, 8, 1, 8);
        rect(x + 33, y, 1, 9, 8);
        text(x + 12, y + 10, ic->ext, 8);
    } else {
        rect(x, y, 50, 35, 10);
    }
    text(x, y + 38, ic->label, 15);
}

static void draw_about(void) {
    window_t* w = &about;
    rect(w->x, w->y + BAR_H, w->w, w->h - BAR_H, 7);
    rect(w->x, w->y, w->w, BAR_H, 9);
    text(w->x + 5, w->y + 2, "About", 15);
    rect(w->x + w->w - 20, w->y + 2, 18, 16, 4);
    text(w->x + w->w - 14, w->y + 2, "X", 15);
    text(w->x + 10, w->y + 30, "RingTail v0.21", 0);
    text(w->x + 10, w->y + 50, "Bare-metal x86 OS", 0);
    text(w->x + 10, w->y + 70, "C + NASM, no libc", 0);
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

    rect(10, 26, 50, 35, 11);
    text(10, 64, "About", 15);
    for (int s = 0; s < PER_PAGE && page * PER_PAGE + s < prog_count; s++) {
        int x, y;
        prog_xy(s, &x, &y);
        draw_icon(&progs[page * PER_PAGE + s], x, y);
    }
    if (about.open) draw_about();
    cursor(m->x, m->y);
    sys_gfx_blit(fb);
}

// Leave graphics, run a program in text mode (a text file runs
// Octavo on it), wait for it, and come back. After a program the user
// presses Enter first, to read what it printed; after a file that
// Octavo closed normally there is nothing to read. Returns 0 to keep
// going.
static int launch(const icon_t* ic) {
    sys_gfx_end();
    const char* prog = ic->ext[0] ? "OCTAVO" : ic->name;
    const char* argv[] = { prog, ic->name, 0 };
    if (!ic->ext[0]) argv[1] = 0;
    int id = sys_spawn_args(prog, argv);
    if (id < 0) {
        printf("kenoma: cannot start %s: %s\n", prog, strerror(id));
    } else {
        int status = sys_join(id);
        if (ic->ext[0] && status == 0) return sys_gfx_begin();
        printf("[%s exited with status %d]\n", prog, status);
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
    window_t* w = &about;
    if (w->open && inside(mx, my, w->x, w->y, w->w, w->h)) {
        if (inside(mx, my, w->x + w->w - 20, w->y + 2, 18, 16)) {
            w->open = 0;
        } else if (my < w->y + BAR_H) {
            dragging = 1;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        }
        return 0;
    }
    if (inside(mx, my, 10, 26, 50, 55)) { w->open = 1; return 0; }
    for (int s = 0; s < PER_PAGE && page * PER_PAGE + s < prog_count; s++) {
        int x, y;
        prog_xy(s, &x, &y);
        const icon_t* ic = &progs[page * PER_PAGE + s];
        if (inside(mx, my, x, y, MAX(50, text_width(ic->label)), 55))
            return launch(ic) != 0;
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = sys_gfx_begin();
    if (r < 0) {
        printf("kenoma: cannot take the display: %s\n", strerror(r));
        return 1;
    }
    sys_gfx_font(font);
    sys_chdir("/");  // the desktop shows the root, where the programs are
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
        if (up) dragging = 0;
        if (dragging) {
            window_t* w = &about;
            w->x = MAX(0, MIN(W - w->w, m.x - drag_dx));
            w->y = MAX(BAR_H, MIN(H - w->h, m.y - drag_dy));
        }
        if (down && !dragging && click(m.x, m.y)) quit = 1;
        if (quit) break;
        draw(&m);
        prev = m;
        if (sys_wait(prev.x, prev.y, prev.buttons) < 0) break;
    }
    sys_gfx_end();
    return 0;
}

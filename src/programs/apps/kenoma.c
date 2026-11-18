// Kenoma, the desktop: a program launcher, text files that open in
// Octavo, a draggable About window, and a mouse cursor, at the screen's
// own resolution (native graphics). Esc or the Quit button leaves.
//
// The desktop is drawn into a scene buffer in this program's memory.
// A frame is the scene with the pointer on top. When only the pointer
// moves, just the two small rectangles where it was and where it is
// now go to the screen; anything else redraws the scene and sends the
// whole of it. Nothing is drawn on the screen twice, so nothing
// flickers.
#include "ulib.h"

#define MAX_PROGS 64
#define SC_ESC 0x01
#define PTR_W 13  // the pointer and its shadow
#define PTR_H 17

static int W, H;              // the screen
static uint32_t* scene;       // the desktop without the pointer
static uint32_t* frame;       // what is on the screen
static uint8_t font[256 * 16];

// Layout, worked out from the screen size: roomier on a big screen,
// the classic sizes on 320x200.
static int big;
static int bar_h, cell_w, cell_h, icon_w, icon_h, grid_x, grid_y, cols, rows;
static int quit_x, quit_w, prev_x, next_x, btn_w, btn_h, btn_y;

// ---- Colours ----
#define C_BLACK   0x000000
#define C_WHITE   0xFFFFFF
#define C_BAR     0xD8DCE2
#define C_BAR_TXT 0x1C2430
#define C_QUIT    0xC0392B
#define C_BTN     0x5A6472
#define C_TITLE   0x2E5C9A
#define C_WINDOW  0xEEF0F3
#define C_SEL     0x6FA8FF
#define C_TERM    0x1E2228
#define C_PROMPT  0x5BE37D
#define C_PAGE    0xFAFAFA
#define C_FOLD    0xC9CED6

// ---- Drawing into the scene ----

static inline void px(uint32_t* b, int x, int y, uint32_t c) {
    if (x >= 0 && x < W && y >= 0 && y < H) b[y * W + x] = c;
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) scene[yy * W + xx] = c;
}

// Mixes c into what is there, a/256 of it: for shadows and highlights.
static void blend(int x, int y, int w, int h, uint32_t c, int a) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            uint32_t d = scene[yy * W + xx], r = 0;
            for (int s = 0; s < 24; s += 8) {
                int dc = (int)(d >> s & 255), cc = (int)(c >> s & 255);
                r |= (uint32_t)(dc + (cc - dc) * a / 256) << s;
            }
            scene[yy * W + xx] = r;
        }
}

static int text_width(const char* s, int scale) { return (int)strlen(s) * 8 * scale; }

static void text(int x, int y, const char* s, uint32_t c, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const uint8_t* g = &font[(uint8_t)*s * 16];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) rect(x + col * scale, y + row * scale, scale, scale, c);
    }
}

// Text with a soft dark copy one pixel down and right, to read on any
// background.
static void label(int x, int y, const char* s, uint32_t c) {
    text(x + 1, y + 1, s, C_BLACK, 1);
    text(x, y, s, c, 1);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// ---- Desktop state ----

typedef struct {
    int x, y, w, h, open;
} window_t;

static window_t about;
static int dragging, drag_dx, drag_dy;
static int hover = -2;  // the icon under the pointer: -1 About, 0.. a slot, -2 none

// Icons: text files first, then programs.
typedef struct {
    char name[NAME_MAX + 1];  // the file name on disk
    char label[13];           // under the icon: the name without its extension
    char ext[4];              // on a page: the extension's first three letters; empty for a program
} icon_t;

static icon_t progs[MAX_PROGS];
static int prog_count, page;

// Slot 0 is About; programs fill the rest of the page.
static int per_page(void) { return cols * rows - 1; }
static int pages(void) { return (prog_count + per_page() - 1) / per_page(); }

static void slot_xy(int slot, int* x, int* y) {
    *x = grid_x + (slot / rows) * cell_w;
    *y = grid_y + (slot % rows) * cell_h;
}

static void layout(void) {
    big = W >= 640 && H >= 400;
    bar_h = big ? 28 : 20;
    btn_h = 18, btn_y = (bar_h - btn_h) / 2;
    cell_w = big ? 104 : 80;
    cell_h = big ? 96 : 58;
    icon_w = big ? 56 : 50;
    icon_h = big ? 44 : 35;
    grid_x = big ? 24 : 10;
    grid_y = bar_h + (big ? 20 : 6);
    cols = MAX(1, (W - grid_x) / cell_w);
    rows = MAX(1, (H - grid_y) / cell_h);
    if (cols * rows < 2) rows = 2;
    quit_w = big ? 56 : 45;
    quit_x = W - quit_w - (big ? 8 : 5);
    btn_w = 18;
    next_x = quit_x - btn_w - (big ? 24 : 12);
    prev_x = next_x - btn_w - 22;
    about.w = big ? 300 : 170;
    about.h = big ? 170 : 100;
    about.x = (W - about.w) / 2;
    about.y = (H - about.h) / 2;
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
    size_t room = big ? sizeof ic->label - 1 : 8;  // what fits under an icon
    if (base > room) {  // a long name: what fits, and a ~
        memcpy(ic->label, name, room - 1);
        ic->label[room - 1] = '~';
        base = room;
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

// ---- The scene ----

static void background(void) {
    // A vertical gradient, deep blue to teal.
    for (int y = bar_h; y < H; y++) {
        int t = (y - bar_h) * 256 / MAX(1, H - bar_h);
        uint32_t r = (uint32_t)(0x10 + (0x14 - 0x10) * t / 256);
        uint32_t g = (uint32_t)(0x2A + (0x6E - 0x2A) * t / 256);
        uint32_t b = (uint32_t)(0x5C + (0x80 - 0x5C) * t / 256);
        uint32_t c = r << 16 | g << 8 | b;
        for (int x = 0; x < W; x++) scene[y * W + x] = c;
    }
}

static void button(int x, int w, uint32_t bg, const char* s) {
    rect(x, btn_y, w, btn_h, bg);
    text(x + (w - text_width(s, 1)) / 2, btn_y + (btn_h - 16) / 2, s, C_WHITE, 1);
}

static void top_bar(const mouse_state_t* m) {
    char buf[32];
    rect(0, 0, W, bar_h, C_BAR);
    rect(0, bar_h - 1, W, 1, 0x9AA2AE);
    int ty = (bar_h - 16) / 2;
    text(8, ty, "RingTail", C_BAR_TXT, 1);
    if (big) {
        rtc_time_t t;
        if (sys_time(&t) == 0) {
            snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", t.year, t.month, t.day, t.hour, t.min);
            text((W - text_width(buf, 1)) / 2, ty, buf, C_BAR_TXT, 1);
        }
    }
    snprintf(buf, sizeof buf, "%d,%d", m->x, m->y);
    text(big ? 96 : 80, ty, buf, 0x5A6472, 1);
    if (pages() > 1) {
        button(prev_x, btn_w, C_BTN, "<");
        snprintf(buf, sizeof buf, "%d", page + 1);
        text(prev_x + btn_w + 3, ty, buf, C_BAR_TXT, 1);
        button(next_x, btn_w, C_BTN, ">");
    }
    button(quit_x, quit_w, C_QUIT, "Quit");
}

// A program: a little terminal window with a prompt in it.
static void program_icon(int x, int y) {
    blend(x + 3, y + 3, icon_w, icon_h, C_BLACK, 90);
    rect(x, y, icon_w, icon_h, C_TERM);
    rect(x, y, icon_w, big ? 7 : 5, 0x8A94A3);
    int s = big ? 2 : 1;
    text(x + 4, y + (big ? 12 : 8), ">", C_PROMPT, 1);
    rect(x + 4 + 8 + 2, y + (big ? 12 : 8) + 13, 6 * s, s, C_PROMPT);
}

// A page with a folded corner, and the file type on it.
static void document_icon(int x, int y, const char* ext) {
    int pw = big ? 40 : 34, f = big ? 10 : 8;
    int px0 = x + (icon_w - pw) / 2;
    blend(px0 + 3, y + 3, pw, icon_h, C_BLACK, 90);
    rect(px0, y, pw, icon_h, C_PAGE);
    rect(px0 + pw - f, y, f, f, scene[MAX(0, y - 1) * W + px0 + pw - 1]);  // cut the corner
    rect(px0 + pw - f, y, 1, f, C_FOLD);
    rect(px0 + pw - f, y + f - 1, f, 1, C_FOLD);
    for (int i = 0; i < 3 && big; i++) rect(px0 + 6, y + 30 + i * 4, pw - 12, 1, C_FOLD);
    text(px0 + (pw - text_width(ext, 1)) / 2, y + (big ? 10 : 10), ext, 0x5A6472, 1);
}

static void about_icon(int x, int y) {
    blend(x + 3, y + 3, icon_w, icon_h, C_BLACK, 90);
    rect(x, y, icon_w, icon_h, 0x4FB3E8);
    int s = big ? 2 : 1;
    text(x + (icon_w - 8 * s) / 2, y + (icon_h - 16 * s) / 2, "i", C_WHITE, s);
}

static void draw_slot(int slot, const char* lab, int kind, const char* ext) {
    int x, y;
    slot_xy(slot, &x, &y);
    int ix = x + (cell_w - icon_w) / 2;
    if (hover == slot - 1) blend(x + 2, y - 4, cell_w - 4, cell_h - 4, C_SEL, 70);
    if (kind == 0) about_icon(ix, y);
    else if (kind == 1) program_icon(ix, y);
    else document_icon(ix, y, ext);
    int lw = text_width(lab, 1);
    label(x + (cell_w - lw) / 2, y + icon_h + (big ? 8 : 3), lab, C_WHITE);
}

static void draw_about(void) {
    window_t* w = &about;
    blend(w->x + 6, w->y + 6, w->w, w->h, C_BLACK, 110);
    rect(w->x, w->y, w->w, w->h, C_WINDOW);
    rect(w->x, w->y, w->w, bar_h, C_TITLE);
    int ty = (bar_h - 16) / 2;
    text(w->x + 8, w->y + ty, "About", C_WHITE, 1);
    rect(w->x + w->w - btn_w - 4, w->y + btn_y, btn_w, btn_h, C_QUIT);
    text(w->x + w->w - btn_w - 4 + 5, w->y + btn_y + (btn_h - 16) / 2, "x", C_WHITE, 1);
    int s = big ? 2 : 1, y = w->y + bar_h + (big ? 14 : 8);
    text(w->x + 12, y, "RingTail OS", C_TITLE, s);
    y += 16 * s + (big ? 12 : 4);
    text(w->x + 12, y, "Bare-metal x86_64", 0x333A44, 1);
    text(w->x + 12, y + 20, "C + NASM, no libc", 0x333A44, 1);
    if (big) {
        char buf[48];
        snprintf(buf, sizeof buf, "Screen %dx%d", W, H);
        text(w->x + 12, y + 40, buf, 0x333A44, 1);
    }
}

static void draw_scene(const mouse_state_t* m) {
    background();
    top_bar(m);
    draw_slot(0, "About", 0, "");
    for (int s = 0; s < per_page() && page * per_page() + s < prog_count; s++) {
        const icon_t* ic = &progs[page * per_page() + s];
        draw_slot(s + 1, ic->label, ic->ext[0] ? 2 : 1, ic->ext);
    }
    if (about.open) draw_about();
}

// ---- The pointer and the screen ----

static const uint16_t cursor_shape[16] = {
    0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xF80, 0xEC0, 0xC60, 0x830, 0x018, 0x000, 0x000, 0x000
};

static void pointer(int x, int y) {
    // Shadow first, then white on top, so black shows only at the edges.
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 12; c++)
            if (cursor_shape[r] & (0x800 >> c)) px(frame, x + c + 1, y + r + 1, C_BLACK);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 12; c++)
            if (cursor_shape[r] & (0x800 >> c)) px(frame, x + c, y + r, C_WHITE);
}

static void put(int x, int y, int w, int h) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    if (x1 <= x0 || y1 <= y0) return;
    gfx_put_t p = { x0, y0, x1 - x0, y1 - y0, W, 0, frame + y0 * W + x0 };
    sys_gfx_put(&p);
}

// Copies part of the scene into the frame.
static void restore(int x, int y, int w, int h) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(W, x + w), y1 = MIN(H, y + h);
    for (int yy = y0; yy < y1; yy++)
        if (x1 > x0) memcpy(frame + yy * W + x0, scene + yy * W + x0, (size_t)(x1 - x0) * 4);
}

static void show_all(const mouse_state_t* m) {
    memcpy(frame, scene, (size_t)W * H * 4);
    pointer(m->x, m->y);
    put(0, 0, W, H);
}

static void show_pointer_move(const mouse_state_t* old, const mouse_state_t* m) {
    restore(old->x, old->y, PTR_W, PTR_H);
    pointer(m->x, m->y);
    put(old->x, old->y, PTR_W, PTR_H);
    put(m->x, m->y, PTR_W, PTR_H);
}

// ---- Doing things ----

static int begin(void) { return sys_gfx_begin_native(); }

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
        if (ic->ext[0] && status == 0) return begin();
        printf("[%s exited with status %d]\n", prog, status);
    }
    print("Press Enter to return to the desktop.");
    char line[8];
    int r = sys_read(STDIN_FILENO, line, sizeof line);
    puts("");
    if (r < 0 && r != -EIO) return -1;
    return begin();
}

// The icon under the pointer: -1 for About, a program's index on the
// page, or -2.
static int icon_at(int mx, int my) {
    if (about.open && inside(mx, my, about.x, about.y, about.w, about.h)) return -2;
    int n = MIN(per_page(), prog_count - page * per_page());
    for (int s = 0; s <= n; s++) {
        int x, y;
        slot_xy(s, &x, &y);
        if (inside(mx, my, x, y - 4, cell_w, cell_h - 4)) return s - 1;
    }
    return -2;
}

// Returns 1 to quit.
static int click(int mx, int my) {
    if (inside(mx, my, quit_x, btn_y, quit_w, btn_h)) return 1;
    if (pages() > 1 && inside(mx, my, prev_x, btn_y, btn_w, btn_h)) {
        page = (page + pages() - 1) % pages();
        return 0;
    }
    if (pages() > 1 && inside(mx, my, next_x, btn_y, btn_w, btn_h)) {
        page = (page + 1) % pages();
        return 0;
    }
    window_t* w = &about;
    if (w->open && inside(mx, my, w->x, w->y, w->w, w->h)) {
        if (inside(mx, my, w->x + w->w - btn_w - 4, w->y + btn_y, btn_w, btn_h)) {
            w->open = 0;
        } else if (my < w->y + bar_h) {
            dragging = 1;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
        }
        return 0;
    }
    int i = icon_at(mx, my);
    if (i == -1) w->open = 1;
    else if (i >= 0) return launch(&progs[page * per_page() + i]) != 0;
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = begin();
    if (r < 0) {
        printf("kenoma: cannot take the display: %s\n", strerror(r));
        return 1;
    }
    gfx_info_t gi;
    sys_gfx_info(&gi);
    W = gi.width, H = gi.height;
    scene = malloc((size_t)W * H * 4);
    frame = malloc((size_t)W * H * 4);
    if (!scene || !frame) {
        sys_gfx_end();
        printf("kenoma: not enough memory for a %dx%d screen\n", W, H);
        return 1;
    }
    sys_gfx_font(font);
    sys_chdir("/");  // the desktop shows the root, where the programs are
    layout();
    load_programs();

    mouse_state_t m, prev;
    sys_mouse_state(&m);
    prev = m;
    hover = icon_at(m.x, m.y);
    draw_scene(&m);
    show_all(&m);
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
        int changed = 0;
        if (up) dragging = 0;
        if (dragging) {
            window_t* w = &about;
            w->x = MAX(0, MIN(W - w->w, m.x - drag_dx));
            w->y = MAX(bar_h, MIN(H - w->h, m.y - drag_dy));
            changed = 1;
        }
        if (down && !dragging) {
            if (click(m.x, m.y)) break;
            changed = 1;  // a window, a page, or the whole screen after a program
        }
        int h = icon_at(m.x, m.y);
        if (h != hover) hover = h, changed = 1;
        if (m.x != prev.x || m.y != prev.y) {
            // The bar shows where the pointer is.
            char buf[32];
            snprintf(buf, sizeof buf, "%d,%d", m.x, m.y);
            int ty = (bar_h - 16) / 2, bx = big ? 96 : 80;
            rect(bx, ty, 8 * 10, 16, C_BAR);
            text(bx, ty, buf, 0x5A6472, 1);
            restore(bx, ty, 8 * 10, 16);
            put(bx, ty, 8 * 10, 16);
        }
        if (changed) {
            draw_scene(&m);
            show_all(&m);
        } else if (m.x != prev.x || m.y != prev.y) {
            show_pointer_move(&prev, &m);
        }
        prev = m;
        if (sys_wait(prev.x, prev.y, prev.buttons) < 0) break;
    }
    sys_gfx_end();
    return 0;
}

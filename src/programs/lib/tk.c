// The toolkit (tk.h). Every drawing call clips to tk->clip, the
// rectangle being drawn, so tk_draw writes nothing outside what it
// returns.
#include "tk.h"
#include "tty.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define DLG_BTN_W 80
#define MENU_PAD  12  // either side of a menu's title

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void scopy(char* d, const char* s, int size) {
    int i = 0;
    for (; s && s[i] && i < size - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static tk_rect_t rect(int x, int y, int w, int h) {
    tk_rect_t r = { x, y, w, h };
    return r;
}

static int inside(tk_rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static tk_rect_t meet(tk_rect_t a, tk_rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return rect(0, 0, 0, 0);
    return rect(x0, y0, x1 - x0, y1 - y0);
}

static int overlaps(tk_rect_t a, tk_rect_t b) { return meet(a, b).w > 0; }

static tk_rect_t wrect(const tk_widget_t* w) { return rect(w->x, w->y, w->w, w->h); }

void tk_dirty(tk_t* tk, int x, int y, int w, int h) {
    tk_rect_t r = meet(rect(x, y, w, h), rect(0, 0, tk->w, tk->h));
    if (!r.w) return;
    if (!tk->dirty.w) {
        tk->dirty = r;
        return;
    }
    int x0 = MIN(tk->dirty.x, r.x), y0 = MIN(tk->dirty.y, r.y);
    int x1 = MAX(tk->dirty.x + tk->dirty.w, r.x + r.w), y1 = MAX(tk->dirty.y + tk->dirty.h, r.y + r.h);
    tk->dirty = rect(x0, y0, x1 - x0, y1 - y0);
}

static void dirty_rect(tk_t* tk, tk_rect_t r) { tk_dirty(tk, r.x, r.y, r.w, r.h); }
static void dirty_widget(tk_t* tk, int id) {
    if (id >= 0 && id < tk->n) dirty_rect(tk, wrect(&tk->wd[id]));
}

// ---- Drawing, clipped ----

static void fill(tk_t* tk, int x, int y, int w, int h, uint32_t c) {
    tk_rect_t r = meet(rect(x, y, w, h), tk->clip);
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        uint32_t* p = tk->px + yy * tk->w + r.x;
        for (int i = 0; i < r.w; i++) p[i] = c;
    }
}

static void border(tk_t* tk, int x, int y, int w, int h, int t, uint32_t c) {
    fill(tk, x, y, w, t, c);
    fill(tk, x, y + h - t, w, t, c);
    fill(tk, x, y, t, h, c);
    fill(tk, x + w - t, y, t, h, c);
}

static void glyph(tk_t* tk, int x, int y, int ch, uint32_t c) {
    const uint8_t* g = &tk->font[(uint8_t)ch * 16];
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col)) {
                int px = x + col, py = y + row;
                if (inside(tk->clip, px, py)) tk->px[py * tk->w + px] = c;
            }
}

// At most max characters of s (n of them, or all with n < 0); when it
// is cut, the last one shown is a ~. Returns the x after it.
static int text_n(tk_t* tk, int x, int y, const char* s, int n, int max, uint32_t c) {
    if (n < 0) n = slen(s);
    if (max < 1) return x;
    int cut = n > max;
    if (cut) n = max;
    for (int i = 0; i < n; i++, x += 8) glyph(tk, x, y, cut && i == n - 1 ? '~' : s[i], c);
    return x;
}

static int text(tk_t* tk, int x, int y, const char* s, int maxw, uint32_t c) {
    return text_n(tk, x, y, s, -1, maxw / 8, c);
}

// ---- Icons ----

static void draw_icon(tk_t* tk, int x, int y, int s, int icon) {
    int u = s / 16;  // 1 or 2
    switch (icon) {
    case TK_ICON_FOLDER:
        fill(tk, x + u, y + 2 * u, 6 * u, 2 * u, 0xD9A23A);           // the tab
        fill(tk, x, y + 4 * u, s, s - 6 * u, 0xB8862B);               // its edge
        fill(tk, x + u, y + 5 * u, s - 2 * u, s - 8 * u, TK_C_FOLDER);
        break;
    case TK_ICON_DOC: {
        int px0 = x + 2 * u, pw = s - 4 * u, f = 4 * u;
        fill(tk, px0, y, pw - f, s, TK_C_LINE);
        fill(tk, px0, y + f, pw, s - f, TK_C_LINE);
        fill(tk, px0 + 1, y + 1, pw - f - 1, s - 2, TK_C_WHITE);
        fill(tk, px0 + 1, y + f, pw - 2, s - f - 1, TK_C_WHITE);
        fill(tk, px0 + pw - f, y + f - 1, f, 1, TK_C_LINE);  // the fold
        fill(tk, px0 + pw - f, y, 1, f, TK_C_LINE);
        for (int i = 0; i < (u > 1 ? 4 : 2); i++) fill(tk, px0 + 3 * u, y + (7 + 3 * i) * u, pw - 6 * u, u > 1 ? 2 : 1, 0xC9CED6);
        break;
    }
    case TK_ICON_PROGRAM:
        fill(tk, x, y + 2 * u, s, s - 4 * u, 0x1E2228);
        fill(tk, x, y + 2 * u, s, 2 * u, TK_C_THUMB);
        if (u > 1) {
            glyph(tk, x + 4, y + 10, '>', 0x5BE37D);
            fill(tk, x + 14, y + 23, 10, 2, 0x5BE37D);
        } else {
            fill(tk, x + 3, y + 7, 2, 2, 0x5BE37D);
            fill(tk, x + 6, y + 10, 5, 1, 0x5BE37D);
        }
        break;
    case TK_ICON_APP:
        fill(tk, x, y + u, s, s - 2 * u, TK_C_TEXT);
        fill(tk, x + 1, y + u + 1, s - 2, 4 * u - 1, TK_C_FOCUS);
        fill(tk, x + 1, y + 5 * u, s - 2, s - 6 * u - 1, TK_C_WHITE);
        fill(tk, x + s - 4 * u, y + 2 * u, 2 * u, 2 * u, 0xC0392B);
        break;
    }
}

void tk_icon(tk_t* tk, int x, int y, int size, int icon) {
    tk_rect_t keep = tk->clip;
    tk->clip = rect(0, 0, tk->w, tk->h);
    draw_icon(tk, x, y, size == 32 ? 32 : 16, icon);
    tk->clip = keep;
}

// ---- Lists: where things are ----

typedef struct {
    tk_rect_t in;    // inside the border, less the scroll bar
    int cols;        // items a row
    int unit;        // a row's height
    int shown;       // rows that fit
    int rows;        // rows there are
    int bar;         // whether there is a scroll bar
    tk_rect_t track; // the scroll bar
} geom_t;

static geom_t geom(const tk_widget_t* w) {
    geom_t g;
    g.in = rect(w->x + 1, w->y + 1, w->w - 2, w->h - 2);
    g.unit = w->layout == TK_ICONS ? TK_CELL_H : TK_ROW_H;
    g.shown = MAX(1, g.in.h / g.unit);
    for (g.bar = 0; g.bar < 2; g.bar++) {
        int iw = g.in.w - (g.bar ? TK_SCROLL_W : 0);
        g.cols = w->layout == TK_ICONS ? MAX(1, iw / TK_CELL_W) : 1;
        g.rows = (w->count + g.cols - 1) / g.cols;
        if (g.rows <= g.shown) break;
    }
    if (g.bar > 1) g.bar = 1;
    if (g.bar) g.in.w -= TK_SCROLL_W;
    g.track = rect(g.in.x + g.in.w, g.in.y, g.bar ? TK_SCROLL_W : 0, g.in.h);
    return g;
}

static int max_top(const geom_t* g) { return MAX(0, g->rows - g->shown); }

static tk_rect_t thumb_rect(const tk_widget_t* w, const geom_t* g) {
    int th = MAX(16, g->track.h * g->shown / MAX(1, g->rows));
    th = MIN(th, g->track.h);
    int span = g->track.h - th, mt = max_top(g);
    int ty = g->track.y + (mt ? w->top * span / mt : 0);
    return rect(g->track.x + 2, ty, g->track.w - 4, th);
}

static tk_rect_t item_rect(const tk_widget_t* w, const geom_t* g, int i) {
    if (i < 0 || i >= w->count) return rect(0, 0, 0, 0);
    int row = i / g->cols - w->top;
    if (row < 0 || row >= g->shown) return rect(0, 0, 0, 0);
    if (w->layout == TK_ICONS) return rect(g->in.x + (i % g->cols) * TK_CELL_W, g->in.y + row * TK_CELL_H, TK_CELL_W, TK_CELL_H);
    return rect(g->in.x, g->in.y + row * TK_ROW_H, g->in.w, TK_ROW_H);
}

tk_rect_t tk_item_rect(const tk_t* tk, int id, int i) {
    if (id < 0 || id >= tk->n || tk->wd[id].type != TK_LIST) return rect(0, 0, 0, 0);
    geom_t g = geom(&tk->wd[id]);
    return item_rect(&tk->wd[id], &g, i);
}

// The item at a point, or -1.
static int item_at(const tk_widget_t* w, const geom_t* g, int x, int y) {
    if (!inside(g->in, x, y)) return -1;
    int col = (x - g->in.x) / (w->layout == TK_ICONS ? TK_CELL_W : g->in.w);
    if (col >= g->cols) return -1;
    int row = (y - g->in.y) / g->unit;
    if (row >= g->shown) return -1;
    int i = (w->top + row) * g->cols + col;
    return i < w->count ? i : -1;
}

static void set_top(tk_t* tk, int id, int top) {
    tk_widget_t* w = &tk->wd[id];
    geom_t g = geom(w);
    top = MAX(0, MIN(top, max_top(&g)));
    if (top != w->top) {
        w->top = top;
        dirty_widget(tk, id);
    }
}

static void show_item(tk_t* tk, int id, int i) {
    tk_widget_t* w = &tk->wd[id];
    if (i < 0) return;
    geom_t g = geom(w);
    int row = i / g.cols;
    if (row < w->top) set_top(tk, id, row);
    else if (row >= w->top + g.shown) set_top(tk, id, row - g.shown + 1);
}

// ---- Menus: where things are ----

static int menu_width(const tk_menu_t* m) {
    int w = 0;
    for (int i = 0; i < m->n && i < TK_MENU_ITEMS; i++) w = MAX(w, slen(m->items[i]));
    return MAX(120, w * 8 + 2 * MENU_PAD + 2);
}

tk_rect_t tk_menu_title_rect(const tk_t* tk, int menu) {
    if (menu < 0 || menu >= tk->nmenus) return rect(0, 0, 0, 0);
    int x = 4;
    for (int i = 0; i < menu; i++) x += slen(tk->menus[i].title) * 8 + 2 * MENU_PAD;
    return rect(x, 0, slen(tk->menus[menu].title) * 8 + 2 * MENU_PAD, TK_BAR_H);
}

// The menu dropped down: under its title, kept in the window.
static tk_rect_t menu_rect(const tk_t* tk, int menu) {
    if (menu < 0 || menu >= tk->nmenus) return rect(0, 0, 0, 0);
    const tk_menu_t* m = &tk->menus[menu];
    tk_rect_t t = tk_menu_title_rect(tk, menu);
    int w = menu_width(m), h = MIN(m->n, TK_MENU_ITEMS) * TK_ITEM_H + 4;
    int x = MAX(0, MIN(t.x, tk->w - w));
    return rect(x, TK_BAR_H - 1, w, h);
}

tk_rect_t tk_menu_item_rect(const tk_t* tk, int menu, int item) {
    tk_rect_t r = menu_rect(tk, menu);
    if (!r.w || item < 0 || item >= MIN(tk->menus[menu].n, TK_MENU_ITEMS)) return rect(0, 0, 0, 0);
    return rect(r.x + 1, r.y + 2 + item * TK_ITEM_H, r.w - 2, TK_ITEM_H);
}

static int separator(const tk_t* tk, int menu, int item) {
    const char* s = tk->menus[menu].items[item];
    return s[0] == '-' && !s[1];
}

// ---- The dialog: where things are ----

static int lines(const char* s, int* longest) {
    int n = 1, len = 0, lo = 0;
    for (; *s; s++) {
        if (*s == '\n') n++, len = 0;
        else lo = MAX(lo, ++len);
    }
    if (longest) *longest = lo;
    return n;
}

static tk_rect_t dialog_rect(const tk_t* tk) {
    int longest, n = lines(tk->dlg_text, &longest);
    int w = MAX(longest * 8 + 40, tk->dlg_n * (DLG_BTN_W + 8) + 32);
    w = MAX(w, tk->dlg_has_field ? 320 : 240);
    w = MIN(w, tk->w - 8);
    int h = 20 + n * 16 + 16 + (tk->dlg_has_field ? TK_LINE_H + 12 : 0) + TK_LINE_H + 16;
    h = MIN(h, tk->h - 8);
    return rect((tk->w - w) / 2, (tk->h - h) / 2, w, h);
}

// The dialog and its shadow.
static tk_rect_t dialog_area(const tk_t* tk) {
    tk_rect_t r = dialog_rect(tk);
    return rect(r.x, r.y, r.w + 4, r.h + 4);
}

tk_rect_t tk_dialog_button_rect(const tk_t* tk, int i) {
    if (!tk->dlg || i < 0 || i >= tk->dlg_n) return rect(0, 0, 0, 0);
    tk_rect_t d = dialog_rect(tk);
    int x = d.x + d.w - 16 - (tk->dlg_n - i) * (DLG_BTN_W + 8) + 8;
    return rect(x, d.y + d.h - 16 - TK_LINE_H, DLG_BTN_W, TK_LINE_H);
}

static void place_dialog_field(tk_t* tk) {
    tk_rect_t d = dialog_rect(tk);
    tk_widget_t* f = &tk->dlg_field;
    f->x = d.x + 16;
    f->y = d.y + 20 + lines(tk->dlg_text, 0) * 16 + 12;
    f->w = d.w - 32;
    f->h = TK_LINE_H;
}

// ---- Making widgets ----

void tk_init(tk_t* tk, uint32_t* pixels, int w, int h, const uint8_t* font) {
    uint8_t* p = (uint8_t*)tk;
    for (unsigned i = 0; i < sizeof *tk; i++) p[i] = 0;
    tk->px = pixels;
    tk->w = w;
    tk->h = h;
    tk->font = font;
    tk->focus = tk->press = tk->thumb = tk->menu = tk->hover = tk->click_list = tk->typed_list = -1;
    tk->active = 1;
    tk_dirty(tk, 0, 0, w, h);
}

static int takes_keys(const tk_widget_t* w) { return !w->hidden && w->type != TK_LABEL; }

static int add(tk_t* tk, int type, int x, int y, int w, int h, const char* text) {
    if (tk->n >= TK_MAX) return -1;
    int id = tk->n++;
    tk_widget_t* d = &tk->wd[id];
    d->type = type;
    d->x = x, d->y = y, d->w = w, d->h = h;
    d->sel = -1;
    scopy(d->text, text, TK_TEXT_MAX);
    d->cur = slen(d->text);
    if (tk->focus < 0 && takes_keys(d)) tk->focus = id;
    dirty_widget(tk, id);
    return id;
}

int tk_label(tk_t* tk, int x, int y, int w, const char* text) {
    return add(tk, TK_LABEL, x, y, w, lines(text ? text : "", 0) * 16, text);
}
int tk_button(tk_t* tk, int x, int y, int w, const char* text) { return add(tk, TK_BUTTON, x, y, w, TK_LINE_H, text); }
int tk_check(tk_t* tk, int x, int y, int w, const char* text, int on) {
    int id = add(tk, TK_CHECK, x, y, w, TK_LINE_H, text);
    if (id >= 0) tk->wd[id].on = on != 0;
    return id;
}
int tk_field(tk_t* tk, int x, int y, int w, const char* text) { return add(tk, TK_FIELD, x, y, w, TK_LINE_H, text); }
int tk_list(tk_t* tk, int x, int y, int w, int h, int layout) {
    int id = add(tk, TK_LIST, x, y, w, h, "");
    if (id >= 0) tk->wd[id].layout = layout;
    return id;
}

void tk_menubar(tk_t* tk, const tk_menu_t* menus, int n) {
    tk->menus = menus;
    tk->nmenus = n;
    tk->menu = tk->hover = -1;
    tk_dirty(tk, 0, 0, tk->w, TK_BAR_H);
}

static int valid(const tk_t* tk, int id) { return id >= 0 && id < tk->n; }

static void field_scroll(tk_widget_t* f) {
    int room = MAX(1, (f->w - 8) / 8 - 1);  // and a place for the cursor at the end
    if (f->cur < f->scroll) f->scroll = f->cur;
    if (f->cur > f->scroll + room) f->scroll = f->cur - room;
    int len = slen(f->text);
    if (f->scroll > 0 && len - f->scroll < room) f->scroll = MAX(0, len - room);
}

void tk_set_text(tk_t* tk, int id, const char* text) {
    if (!valid(tk, id)) return;
    tk_widget_t* w = &tk->wd[id];
    dirty_widget(tk, id);  // a label may get shorter
    scopy(w->text, text, TK_TEXT_MAX);
    if (w->type == TK_LABEL) w->h = lines(w->text, 0) * 16;
    w->cur = slen(w->text);
    w->scroll = 0;
    field_scroll(w);
    dirty_widget(tk, id);
}

const char* tk_text(const tk_t* tk, int id) { return valid(tk, id) ? tk->wd[id].text : ""; }

void tk_set_on(tk_t* tk, int id, int on) {
    if (!valid(tk, id)) return;
    tk->wd[id].on = on != 0;
    dirty_widget(tk, id);
}

void tk_set_focus(tk_t* tk, int id) {
    if (id >= 0 && (!valid(tk, id) || !takes_keys(&tk->wd[id]))) return;
    if (id == tk->focus) return;
    dirty_widget(tk, tk->focus);
    tk->focus = id;
    dirty_widget(tk, id);
}

void tk_hide(tk_t* tk, int id, int hidden) {
    if (!valid(tk, id)) return;
    tk->wd[id].hidden = hidden != 0;
    dirty_widget(tk, id);
    if (hidden && tk->focus == id) tk->focus = -1;
}

void tk_list_set(tk_t* tk, int id, const tk_item_t* items, int count) {
    if (!valid(tk, id) || tk->wd[id].type != TK_LIST) return;
    tk_widget_t* w = &tk->wd[id];
    w->items = items;
    w->count = MAX(0, count);
    w->sel = w->count ? 0 : -1;
    w->top = 0;
    if (tk->click_list == id) tk->click_list = -1;
    dirty_widget(tk, id);
}

void tk_list_layout(tk_t* tk, int id, int layout) {
    if (!valid(tk, id) || tk->wd[id].type != TK_LIST) return;
    tk->wd[id].layout = layout;
    tk->wd[id].top = 0;
    show_item(tk, id, tk->wd[id].sel);
    dirty_widget(tk, id);
}

void tk_list_select(tk_t* tk, int id, int i) {
    if (!valid(tk, id) || tk->wd[id].type != TK_LIST) return;
    tk_widget_t* w = &tk->wd[id];
    if (i < -1 || i >= w->count) return;
    w->sel = i;
    show_item(tk, id, i);
    dirty_widget(tk, id);
}

void tk_dialog(tk_t* tk, int tag, const char* text, const char* const* buttons, int n, const char* field) {
    n = MAX(1, MIN(n, TK_DLG_BTNS));
    if (tk->dlg) dirty_rect(tk, dialog_area(tk));
    tk->dlg = 1;
    tk->dlg_tag = tag;
    scopy(tk->dlg_text, text, TK_DLG_MAX);
    for (int i = 0; i < n; i++) tk->dlg_btn[i] = buttons[i];
    tk->dlg_n = n;
    tk->dlg_sel = 0;
    tk->dlg_armed = -1;
    tk->dlg_has_field = field != 0;
    tk_widget_t* f = &tk->dlg_field;
    for (unsigned i = 0; i < sizeof *f; i++) ((uint8_t*)f)[i] = 0;
    f->type = TK_FIELD;
    scopy(f->text, field, TK_TEXT_MAX);
    f->cur = slen(f->text);
    place_dialog_field(tk);
    field_scroll(f);
    // A menu or a press in progress ends here.
    if (tk->menu >= 0) dirty_rect(tk, menu_rect(tk, tk->menu));
    tk->menu = tk->hover = -1;
    tk->press = tk->thumb = -1;
    tk->armed = 0;
    dirty_rect(tk, dialog_area(tk));
}

const char* tk_dialog_text(const tk_t* tk) { return tk->dlg_field.text; }

// ---- Drawing widgets ----

static void draw_lines(tk_t* tk, int x, int y, int w, const char* s) {
    for (;; y += 16) {
        int n = 0;
        while (s[n] && s[n] != '\n') n++;
        text_n(tk, x, y, s, n, w / 8, TK_C_TEXT);
        s += n;
        if (!*s) break;
        s++;
    }
}

static int focused(const tk_t* tk, int id) { return tk->active && tk->focus == id && !tk->dlg; }

static void draw_button(tk_t* tk, const tk_widget_t* w, int down, int focus) {
    fill(tk, w->x, w->y, w->w, w->h, down ? TK_C_DOWN : TK_C_FACE);
    border(tk, w->x, w->y, w->w, w->h, 1, TK_C_LINE);
    if (focus) border(tk, w->x, w->y, w->w, w->h, 2, TK_C_FOCUS);
    int tw = MIN(slen(w->text) * 8, w->w - 8);
    text(tk, w->x + (w->w - tw) / 2 + down, w->y + (w->h - 16) / 2 + down, w->text, w->w - 8, TK_C_TEXT);
}

static void draw_check(tk_t* tk, int id, const tk_widget_t* w) {
    int bx = w->x + 2, by = w->y + (w->h - 14) / 2;
    fill(tk, bx, by, 14, 14, TK_C_WHITE);
    border(tk, bx, by, 14, 14, 1, TK_C_LINE);
    if (focused(tk, id)) border(tk, bx - 2, by - 2, 18, 18, 2, TK_C_FOCUS);
    if (w->on)  // a tick: down to the bottom, then up to the right
        for (int i = 0; i < 9; i++) {
            int x = bx + 3 + i, y = i < 3 ? by + 6 + i : by + 8 - (i - 3);
            fill(tk, x, y, 1, 3, TK_C_FOCUS);
        }
    text(tk, w->x + 24, w->y + (w->h - 16) / 2, w->text, w->w - 24, TK_C_TEXT);
}

static void draw_field(tk_t* tk, const tk_widget_t* w, int focus) {
    fill(tk, w->x, w->y, w->w, w->h, TK_C_WHITE);
    border(tk, w->x, w->y, w->w, w->h, focus ? 2 : 1, focus ? TK_C_FOCUS : TK_C_LINE);
    tk_rect_t keep = tk->clip;
    tk->clip = meet(keep, rect(w->x + 2, w->y + 2, w->w - 4, w->h - 4));
    int ty = w->y + (w->h - 16) / 2;
    int n = slen(w->text) - w->scroll;
    text_n(tk, w->x + 4, ty, w->text + w->scroll, n, n, TK_C_TEXT);
    if (focus) fill(tk, w->x + 4 + (w->cur - w->scroll) * 8 - 1, ty, 2, 16, TK_C_TEXT);
    tk->clip = keep;
}

static void draw_list(tk_t* tk, int id, const tk_widget_t* w) {
    int focus = focused(tk, id);
    fill(tk, w->x, w->y, w->w, w->h, TK_C_WHITE);
    border(tk, w->x, w->y, w->w, w->h, 1, focus ? TK_C_FOCUS : TK_C_LINE);
    geom_t g = geom(w);
    tk_rect_t keep = tk->clip;
    tk->clip = meet(keep, g.in);
    uint32_t selbg = focus ? TK_C_FOCUS : TK_C_SEL, selfg = focus ? TK_C_WHITE : TK_C_TEXT;
    for (int i = w->top * g.cols; i < w->count && i < (w->top + g.shown) * g.cols; i++) {
        tk_rect_t r = item_rect(w, &g, i);
        if (!overlaps(r, tk->clip)) continue;
        const tk_item_t* it = &w->items[i];
        const char* name = it->text ? it->text : "";
        int sel = i == w->sel;
        if (w->layout == TK_ICONS) {
            // A name keeps 4 pixels clear on each side, so two long
            // names side by side don't read as one.
            int tw = MIN(slen(name), (TK_CELL_W - 8) / 8) * 8;
            int lx = r.x + (TK_CELL_W - tw) / 2, ly = r.y + 44;
            if (sel) {
                fill(tk, r.x + (TK_CELL_W - 40) / 2, r.y + 2, 40, 40, TK_C_SEL);
                fill(tk, lx - 3, ly - 1, tw + 6, 18, selbg);
            }
            draw_icon(tk, r.x + (TK_CELL_W - 32) / 2, r.y + 6, 32, it->icon);
            text(tk, lx, ly, name, TK_CELL_W - 8, sel ? selfg : TK_C_TEXT);
        } else {
            if (sel) fill(tk, r.x, r.y, r.w, r.h, selbg);
            int tx = r.x + 4;
            if (it->icon) {
                draw_icon(tk, tx, r.y + 2, 16, it->icon);
                tx += 22;
            }
            int nw = it->note ? slen(it->note) * 8 : 0;
            int room = r.x + r.w - tx - (nw ? nw + 12 : 4);
            text(tk, tx, r.y + 2, name, room, sel ? selfg : TK_C_TEXT);
            if (nw) text(tk, r.x + r.w - 6 - nw, r.y + 2, it->note, nw, sel ? selfg : TK_C_DIM);
        }
    }
    tk->clip = keep;
    if (g.bar) {
        fill(tk, g.track.x, g.track.y, g.track.w, g.track.h, TK_C_FACE);
        fill(tk, g.track.x, g.track.y, 1, g.track.h, TK_C_LINE);
        tk_rect_t t = thumb_rect(w, &g);
        fill(tk, t.x + 1, t.y, t.w - 1, t.h, TK_C_THUMB);
    }
}

static void draw_menubar(tk_t* tk) {
    fill(tk, 0, 0, tk->w, TK_BAR_H - 1, TK_C_BAR);
    fill(tk, 0, TK_BAR_H - 1, tk->w, 1, 0xB8BEC8);
    for (int i = 0; i < tk->nmenus; i++) {
        tk_rect_t r = tk_menu_title_rect(tk, i);
        int open = i == tk->menu;
        if (open) fill(tk, r.x, r.y, r.w, r.h - 1, TK_C_FOCUS);
        text(tk, r.x + MENU_PAD, (TK_BAR_H - 16) / 2, tk->menus[i].title, r.w, open ? TK_C_WHITE : TK_C_TEXT);
    }
}

static void draw_menu(tk_t* tk) {
    tk_rect_t r = menu_rect(tk, tk->menu);
    fill(tk, r.x, r.y, r.w, r.h, TK_C_WHITE);
    border(tk, r.x, r.y, r.w, r.h, 1, TK_C_LINE);
    const tk_menu_t* m = &tk->menus[tk->menu];
    for (int i = 0; i < m->n && i < TK_MENU_ITEMS; i++) {
        tk_rect_t it = tk_menu_item_rect(tk, tk->menu, i);
        if (separator(tk, tk->menu, i)) {
            fill(tk, it.x + 6, it.y + it.h / 2, it.w - 12, 1, 0xC9CED6);
            continue;
        }
        int lit = i == tk->hover;
        if (lit) fill(tk, it.x, it.y, it.w, it.h, TK_C_FOCUS);
        text(tk, it.x + MENU_PAD, it.y + (it.h - 16) / 2, m->items[i], it.w - MENU_PAD, lit ? TK_C_WHITE : TK_C_TEXT);
    }
}

static void draw_dialog(tk_t* tk) {
    tk_rect_t d = dialog_rect(tk);
    fill(tk, d.x + 4, d.y + d.h, d.w, 4, 0x9AA2AE);  // a shadow
    fill(tk, d.x + d.w, d.y + 4, 4, d.h, 0x9AA2AE);
    fill(tk, d.x, d.y, d.w, d.h, 0xF4F5F7);
    border(tk, d.x, d.y, d.w, d.h, 1, TK_C_TEXT);
    fill(tk, d.x + 1, d.y + 1, d.w - 2, 4, TK_C_FOCUS);
    draw_lines(tk, d.x + 16, d.y + 20, d.w - 32, tk->dlg_text);
    if (tk->dlg_has_field) draw_field(tk, &tk->dlg_field, tk->active);
    for (int i = 0; i < tk->dlg_n; i++) {
        tk_widget_t b = { 0 };
        tk_rect_t r = tk_dialog_button_rect(tk, i);
        b.x = r.x, b.y = r.y, b.w = r.w, b.h = r.h;
        scopy(b.text, tk->dlg_btn[i], TK_TEXT_MAX);
        draw_button(tk, &b, tk->dlg_armed == i, i == tk->dlg_sel && tk->active);
    }
}

tk_rect_t tk_draw(tk_t* tk) {
    tk_rect_t d = tk->dirty;
    tk->dirty = rect(0, 0, 0, 0);
    if (!d.w) return d;
    tk->clip = d;
    fill(tk, d.x, d.y, d.w, d.h, TK_C_BG);
    for (int id = 0; id < tk->n; id++) {
        const tk_widget_t* w = &tk->wd[id];
        if (w->hidden || !overlaps(wrect(w), d)) continue;
        switch (w->type) {
        case TK_LABEL: draw_lines(tk, w->x, w->y, w->w, w->text); break;
        case TK_BUTTON: draw_button(tk, w, tk->press == id && tk->armed, focused(tk, id)); break;
        case TK_CHECK: draw_check(tk, id, w); break;
        case TK_FIELD: draw_field(tk, w, focused(tk, id)); break;
        case TK_LIST: draw_list(tk, id, w); break;
        }
    }
    if (tk->nmenus && overlaps(rect(0, 0, tk->w, TK_BAR_H), d)) draw_menubar(tk);
    if (tk->menu >= 0 && overlaps(menu_rect(tk, tk->menu), d)) draw_menu(tk);
    if (tk->dlg && overlaps(dialog_area(tk), d)) draw_dialog(tk);
    tk->clip = rect(0, 0, tk->w, tk->h);
    return d;
}

// ---- Events ----

static int emit(tk_event_t* ev, int type, int id, int a, int b) {
    ev->type = type, ev->id = id, ev->a = a, ev->b = b;
    return 1;
}

// Editing a field with a key; 1 if it changed the text, 0 if it only
// moved the cursor, -1 if the key is not one for a field.
static int field_key(tk_widget_t* f, int code) {
    int len = slen(f->text);
    if (code >= 0x20 && code < 0x7F) {
        if (len >= TK_TEXT_MAX - 1) return 0;
        for (int i = len; i >= f->cur; i--) f->text[i + 1] = f->text[i];
        f->text[f->cur++] = (char)code;
        field_scroll(f);
        return 1;
    }
    if (code == '\b') {
        if (!f->cur) return 0;
        for (int i = f->cur - 1; i < len; i++) f->text[i] = f->text[i + 1];
        f->cur--;
        field_scroll(f);
        return 1;
    }
    if (!KEY_IS_NAV(code)) return -1;
    switch (KEY_NAV_KEY(code)) {
    case NAV_DELETE:
        if (f->cur >= len) return 0;
        for (int i = f->cur; i < len; i++) f->text[i] = f->text[i + 1];
        field_scroll(f);
        return 1;
    case NAV_LEFT: f->cur = MAX(0, f->cur - 1); break;
    case NAV_RIGHT: f->cur = MIN(len, f->cur + 1); break;
    case NAV_HOME: f->cur = 0; break;
    case NAV_END: f->cur = len; break;
    default: return -1;
    }
    field_scroll(f);
    return 0;
}

static void field_click(tk_widget_t* f, int x) {
    int c = f->scroll + (x - f->x - 4 + 4) / 8;
    f->cur = MAX(0, MIN(c, slen(f->text)));
    field_scroll(f);
}

static void menu_open(tk_t* tk, int menu, int hover) {
    if (tk->menu >= 0) dirty_rect(tk, menu_rect(tk, tk->menu));
    tk->menu = menu;
    tk->hover = hover;
    tk_dirty(tk, 0, 0, tk->w, TK_BAR_H);
    if (menu >= 0) dirty_rect(tk, menu_rect(tk, menu));
}

static void menu_hover(tk_t* tk, int item) {
    if (item == tk->hover) return;
    tk->hover = item;
    dirty_rect(tk, menu_rect(tk, tk->menu));
}

// The next item of the open menu from item, step +1 or -1, past lines.
static int menu_step(const tk_t* tk, int item, int step) {
    int n = MIN(tk->menus[tk->menu].n, TK_MENU_ITEMS);
    for (int k = 0; k < n; k++) {
        item = item < 0 ? (step > 0 ? 0 : n - 1) : (item + step + n) % n;
        if (!separator(tk, tk->menu, item)) return item;
    }
    return -1;
}

static int menu_choose(tk_t* tk, int item, tk_event_t* ev) {
    int menu = tk->menu;
    menu_open(tk, -1, -1);
    if (item < 0 || separator(tk, menu, item)) return 0;
    return emit(ev, TK_EV_MENU, -1, menu, item);
}

static int title_at(const tk_t* tk, int x, int y) {
    for (int i = 0; i < tk->nmenus; i++)
        if (inside(tk_menu_title_rect(tk, i), x, y)) return i;
    return -1;
}

static int menu_item_at(const tk_t* tk, int x, int y) {
    if (tk->menu < 0) return -1;
    for (int i = 0; i < MIN(tk->menus[tk->menu].n, TK_MENU_ITEMS); i++)
        if (inside(tk_menu_item_rect(tk, tk->menu, i), x, y)) return separator(tk, tk->menu, i) ? -1 : i;
    return -1;
}

static int answer(tk_t* tk, int i, tk_event_t* ev) {
    dirty_rect(tk, dialog_area(tk));
    tk->dlg = 0;
    tk->press = -1;
    dirty_widget(tk, tk->focus);
    return emit(ev, TK_EV_ANSWER, tk->dlg_tag, i, 0);
}

static int dialog_mouse(tk_t* tk, int x, int y, int pressed, int released, tk_event_t* ev) {
    tk_rect_t d = dialog_rect(tk);
    int on = -1;
    for (int i = 0; i < tk->dlg_n; i++)
        if (inside(tk_dialog_button_rect(tk, i), x, y)) on = i;
    if (pressed) {
        if (on >= 0) {
            tk->dlg_armed = on;
            tk->dlg_sel = on;
            dirty_rect(tk, d);
        } else if (tk->dlg_has_field && inside(wrect(&tk->dlg_field), x, y)) {
            field_click(&tk->dlg_field, x);
            dirty_rect(tk, wrect(&tk->dlg_field));
        }
        return 0;
    }
    if (tk->dlg_armed >= 0) {
        int a = tk->dlg_armed;
        if (released) {
            tk->dlg_armed = -1;
            dirty_rect(tk, d);
            if (on == a) return answer(tk, a, ev);
        }
    }
    return 0;
}

static int list_mouse(tk_t* tk, int id, int x, int y, tk_event_t* ev) {
    tk_widget_t* w = &tk->wd[id];
    geom_t g = geom(w);
    if (g.bar && inside(g.track, x, y)) {
        tk_rect_t t = thumb_rect(w, &g);
        if (y < t.y) set_top(tk, id, w->top - g.shown);
        else if (y >= t.y + t.h) set_top(tk, id, w->top + g.shown);
        else tk->thumb = id, tk->thumb_dy = y - t.y;
        return 0;
    }
    int i = item_at(w, &g, x, y);
    if (i < 0) return 0;
    int twice = tk->click_list == id && tk->click_item == i && tk->now - tk->click_at < TK_DCLICK;
    tk->click_list = twice ? -1 : id;
    tk->click_item = i;
    tk->click_at = tk->now;
    if (twice) return emit(ev, TK_EV_OPEN, id, i, 0);
    if (i == w->sel) return 0;
    w->sel = i;
    dirty_widget(tk, id);
    return emit(ev, TK_EV_SELECT, id, i, 0);
}

static void thumb_drag(tk_t* tk, int y) {
    tk_widget_t* w = &tk->wd[tk->thumb];
    geom_t g = geom(w);
    tk_rect_t t = thumb_rect(w, &g);
    int span = g.track.h - t.h, mt = max_top(&g);
    if (span <= 0) return;
    int pos = y - tk->thumb_dy - g.track.y;
    set_top(tk, tk->thumb, (pos * mt + span / 2) / span);
}

static int widget_at(const tk_t* tk, int x, int y) {
    for (int id = tk->n - 1; id >= 0; id--)
        if (!tk->wd[id].hidden && inside(wrect(&tk->wd[id]), x, y)) return id;
    return -1;
}

static int mouse(tk_t* tk, int x, int y, int buttons, tk_event_t* ev) {
    int pressed = buttons & ~tk->buttons & 1, released = tk->buttons & ~buttons & 1;
    tk->buttons = buttons;
    if (tk->dlg) return dialog_mouse(tk, x, y, pressed, released, ev);

    if (tk->menu >= 0) {
        int t = title_at(tk, x, y), item = menu_item_at(tk, x, y);
        if (t >= 0 && t != tk->menu) menu_open(tk, t, -1);
        else if (inside(menu_rect(tk, tk->menu), x, y)) menu_hover(tk, item);
        if (released && item >= 0) return menu_choose(tk, item, ev);
        if (pressed && t == tk->menu && tk->press != -2) menu_open(tk, -1, -1);  // a second click on its title
        else if (pressed && t < 0 && !inside(menu_rect(tk, tk->menu), x, y)) menu_open(tk, -1, -1);
        if (released) tk->press = -1;
        return 0;
    }
    if (pressed) {
        int t = title_at(tk, x, y);
        if (t >= 0) {
            menu_open(tk, t, -1);
            tk->press = -2;  // the press that opened it: its release leaves it open
            return 0;
        }
        int id = widget_at(tk, x, y);
        tk->press = id;
        if (id < 0) return 0;
        tk_widget_t* w = &tk->wd[id];
        if (takes_keys(w)) tk_set_focus(tk, id);
        switch (w->type) {
        case TK_BUTTON:
        case TK_CHECK:
            tk->armed = 1;
            dirty_widget(tk, id);
            return 0;
        case TK_FIELD:
            field_click(w, x);
            dirty_widget(tk, id);
            return 0;
        case TK_LIST:
            return list_mouse(tk, id, x, y, ev);
        }
        return 0;
    }
    if (tk->thumb >= 0) {
        if (buttons & 1) thumb_drag(tk, y);
        else tk->thumb = -1;
        return 0;
    }
    int id = tk->press;
    if (id >= 0 && (tk->wd[id].type == TK_BUTTON || tk->wd[id].type == TK_CHECK)) {
        tk_widget_t* w = &tk->wd[id];
        int on = inside(wrect(w), x, y);
        if (released) {
            int was = tk->armed;
            tk->press = -1;
            tk->armed = 0;
            dirty_widget(tk, id);
            if (!was || !on) return 0;
            if (w->type == TK_BUTTON) return emit(ev, TK_EV_CLICK, id, 0, 0);
            w->on = !w->on;
            return emit(ev, TK_EV_TOGGLE, id, w->on, 0);
        }
        if (on != tk->armed) {
            tk->armed = on;
            dirty_widget(tk, id);
        }
        return 0;
    }
    if (released) tk->press = -1;
    return 0;
}

static void focus_step(tk_t* tk, int step) {
    for (int k = 1; k <= tk->n; k++) {
        int id = ((tk->focus < 0 ? (step > 0 ? -1 : 0) : tk->focus) + step * k + tk->n * 2) % tk->n;
        if (takes_keys(&tk->wd[id])) {
            tk_set_focus(tk, id);
            return;
        }
    }
}

// The item typed letters go to. Letters that come within TK_TYPEAHEAD
// of each other spell the start of a name: the first item, from the
// selected one on, that begins with them. One letter, or the same one
// again and again, steps to the next item starting with it.
static int list_letter(tk_t* tk, int id, int c) {
    const tk_widget_t* w = &tk->wd[id];
    if (tk->typed_list != id || tk->now - tk->typed_at >= TK_TYPEAHEAD || tk->typed_n == (int)sizeof tk->typed)
        tk->typed_n = 0;
    tk->typed_list = id;
    tk->typed_at = tk->now;
    tk->typed[tk->typed_n++] = (char)lower(c);
    int n = 1;
    for (int k = 1; k < tk->typed_n; k++)
        if (tk->typed[k] != tk->typed[0]) n = tk->typed_n;
    int start = n == 1 ? w->sel + 1 : MAX(w->sel, 0);
    for (int k = 0; k < w->count; k++) {
        int i = (start + k) % w->count;
        const char* s = w->items[i].text;
        int j = 0;
        while (j < n && s && s[j] && lower(s[j]) == tk->typed[j]) j++;
        if (j == n) return i;
    }
    return -1;
}

static int list_key(tk_t* tk, int id, int code, tk_event_t* ev) {
    tk_widget_t* w = &tk->wd[id];
    if (code == '\n') return w->sel >= 0 ? emit(ev, TK_EV_OPEN, id, w->sel, 0) : 0;
    geom_t g = geom(w);
    int i = w->sel, last = w->count - 1;
    if (code > 0x20 && code < 0x7F) {
        i = list_letter(tk, id, code);
        if (i < 0) return 0;
    } else if (KEY_IS_NAV(code)) {
        int across = w->layout == TK_ICONS;
        switch (KEY_NAV_KEY(code)) {
        case NAV_UP: i = i < 0 ? 0 : i - g.cols; break;
        case NAV_DOWN: i = i < 0 ? 0 : (i + g.cols <= last ? i + g.cols : i); break;
        case NAV_LEFT: if (!across) return -1; i = i < 0 ? 0 : i - 1; break;
        case NAV_RIGHT: if (!across) return -1; i = i < 0 ? 0 : i + 1; break;
        case NAV_HOME: i = 0; break;
        case NAV_END: i = last; break;
        case NAV_PGUP: i = i < 0 ? 0 : i - g.cols * g.shown; break;
        case NAV_PGDN: i = i < 0 ? 0 : i + g.cols * g.shown; break;
        default: return -1;
        }
        i = MAX(0, MIN(i, last));
    } else {
        return -1;
    }
    if (w->count == 0) return 0;
    show_item(tk, id, i);
    if (i == w->sel) return 0;
    w->sel = i;
    dirty_widget(tk, id);
    return emit(ev, TK_EV_SELECT, id, i, 0);
}

static int dialog_key(tk_t* tk, int code, int mods, tk_event_t* ev) {
    if (code == '\n') return answer(tk, tk->dlg_sel, ev);
    if (code == 0x1B) return answer(tk, tk->dlg_n - 1, ev);
    if (code == '\t') {
        tk->dlg_sel = (tk->dlg_sel + (mods & KEY_MOD_SHIFT ? tk->dlg_n - 1 : 1)) % tk->dlg_n;
        dirty_rect(tk, dialog_rect(tk));
        return 0;
    }
    if (tk->dlg_has_field && field_key(&tk->dlg_field, code) >= 0) {
        dirty_rect(tk, wrect(&tk->dlg_field));
        return 0;
    }
    if (!tk->dlg_has_field && KEY_IS_NAV(code)) {
        int k = KEY_NAV_KEY(code);
        if (k == NAV_LEFT || k == NAV_RIGHT) {
            tk->dlg_sel = (tk->dlg_sel + (k == NAV_LEFT ? tk->dlg_n - 1 : 1)) % tk->dlg_n;
            dirty_rect(tk, dialog_rect(tk));
        }
    }
    return 0;
}

static int menu_key(tk_t* tk, int code, tk_event_t* ev) {
    if (code == 0x1B) {
        menu_open(tk, -1, -1);
        return 0;
    }
    if (code == '\n') return menu_choose(tk, tk->hover, ev);
    if (!KEY_IS_NAV(code)) return 0;
    switch (KEY_NAV_KEY(code)) {
    case NAV_UP: menu_hover(tk, menu_step(tk, tk->hover, -1)); break;
    case NAV_DOWN: menu_hover(tk, menu_step(tk, tk->hover, 1)); break;
    case NAV_LEFT: menu_open(tk, (tk->menu + tk->nmenus - 1) % tk->nmenus, -1); break;
    case NAV_RIGHT: menu_open(tk, (tk->menu + 1) % tk->nmenus, -1); break;
    }
    return 0;
}

static int key(tk_t* tk, int code, int mods, tk_event_t* ev) {
    if (tk->dlg) return dialog_key(tk, code, mods, ev);
    if (tk->menu >= 0) return menu_key(tk, code, ev);
    if (code == KEY_NAV(NAV_F1 + 9, 0) && tk->nmenus) {  // F10: the menus
        menu_open(tk, 0, -1);
        menu_hover(tk, menu_step(tk, -1, 1));
        return 0;
    }
    if (code == '\t' && !(mods & KEY_MOD_CTRL)) {
        focus_step(tk, mods & KEY_MOD_SHIFT ? -1 : 1);
        return 0;
    }
    int id = tk->focus;
    if (id >= 0) {
        tk_widget_t* w = &tk->wd[id];
        switch (w->type) {
        case TK_BUTTON:
            if (code == '\n' || code == ' ') return emit(ev, TK_EV_CLICK, id, 0, 0);
            break;
        case TK_CHECK:
            if (code == ' ') {
                w->on = !w->on;
                dirty_widget(tk, id);
                return emit(ev, TK_EV_TOGGLE, id, w->on, 0);
            }
            break;
        case TK_FIELD: {
            if (code == '\n') return emit(ev, TK_EV_SUBMIT, id, 0, 0);
            if (mods & (KEY_MOD_CTRL | 4)) break;  // 4: Alt (keymap.h)
            int r = field_key(w, code);
            if (r < 0) break;
            dirty_widget(tk, id);
            return r ? emit(ev, TK_EV_CHANGE, id, 0, 0) : 0;
        }
        case TK_LIST: {
            if (mods & (KEY_MOD_CTRL | 4)) break;
            int r = list_key(tk, id, code, ev);
            if (r >= 0) return r;
            break;
        }
        }
    }
    return code ? emit(ev, TK_EV_KEY, -1, code, mods) : 0;
}

int tk_handle(tk_t* tk, const ws_msg_t* m, tk_event_t* ev) {
    ev->type = TK_EV_NONE;
    switch (m->type) {
    case WS_MOUSE:
        return mouse(tk, m->mouse.x, m->mouse.y, m->mouse.buttons, ev);
    case WS_KEY:
        if (!m->key.down) return 0;
        return key(tk, m->key.code, m->key.mods, ev);
    case WS_FOCUS:
        tk->active = m->focus.on != 0;
        dirty_widget(tk, tk->focus);
        if (tk->dlg) dirty_rect(tk, dialog_rect(tk));
        if (!tk->active && tk->menu >= 0) menu_open(tk, -1, -1);
        return 0;
    }
    return 0;
}

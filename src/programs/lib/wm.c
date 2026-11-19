// The window manager's core (wm.h). Every drawing call clips to the
// rectangle being composed, so a compose writes nothing outside it.
#include "wm.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void scopy(char* d, const char* s, int size) {
    int i = 0;
    for (; s[i] && i < size - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static wm_rect_t clip(wm_rect_t a, wm_rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    wm_rect_t r = { x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0) };
    if (!r.w || !r.h) r.w = r.h = 0;
    return r;
}

static int inside(wm_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static wm_rect_t rect(int x, int y, int w, int h) {
    wm_rect_t r = { x, y, w, h };
    return r;
}

// ---- Geometry ----

wm_rect_t wm_frame(const wm_t* wm, int win) {
    const wm_win_t* w = &wm->win[win];
    return rect(w->x - WM_BORDER, w->y - WM_TITLE_H - WM_BORDER, w->w + 2 * WM_BORDER,
                w->h + WM_TITLE_H + 2 * WM_BORDER);
}

// The frame and its shadow: what moving or removing a window uncovers.
static wm_rect_t outer(const wm_t* wm, int win) {
    wm_rect_t f = wm_frame(wm, win);
    f.w += WM_SHADOW;
    f.h += WM_SHADOW;
    return f;
}

static wm_rect_t title_rect(const wm_t* wm, int win) {
    wm_rect_t f = wm_frame(wm, win);
    return rect(f.x + WM_BORDER, f.y + WM_BORDER, f.w - 2 * WM_BORDER, WM_TITLE_H);
}

static wm_rect_t close_rect(const wm_t* wm, int win) {
    wm_rect_t t = title_rect(wm, win);
    return rect(t.x + t.w - WM_CLOSE_W - 3, t.y + (WM_TITLE_H - WM_CLOSE_W) / 2, WM_CLOSE_W, WM_CLOSE_W);
}

static wm_rect_t content_rect(const wm_t* wm, int win) {
    const wm_win_t* w = &wm->win[win];
    return rect(w->x, w->y, w->w, w->h);
}

static wm_rect_t menu_button(void) { return rect(4, 3, 88, WM_BAR_H - 6); }

// The bar's buttons go in table order, which does not change as
// windows are raised; slot is how many used windows come before.
static wm_rect_t bar_button(int slot) {
    return rect(WM_BTN_X + slot * (WM_BTN_W + 4), 3, WM_BTN_W, WM_BAR_H - 6);
}

static int clock_x(const wm_t* wm) { return wm->W - 8 - 8 * slen(wm->clock); }

// Room for window buttons: up to the clock.
static int bar_slots(const wm_t* wm) {
    int room = clock_x(wm) - 12 - WM_BTN_X;
    return room < 0 ? 0 : (room + 4) / (WM_BTN_W + 4);
}

wm_rect_t wm_menu_rect(const wm_t* wm) {
    return rect(wm->menu_x, wm->menu_y, WM_MENU_W, wm->menu_n * WM_ITEM_H + 4);
}

int wm_top(const wm_t* wm) { return wm->n ? wm->order[wm->n - 1] : -1; }

// ---- Damage ----

void wm_damage(wm_t* wm, int x, int y, int w, int h) {
    wm_rect_t r = clip(rect(x, y, w, h), rect(0, 0, wm->W, wm->H));
    if (!r.w) return;
    // Merge with anything it overlaps or touches, again and again, as
    // each merge grows it.
    for (int i = 0; i < wm->ndamage;) {
        wm_rect_t d = wm->damage[i];
        if (r.x <= d.x + d.w && d.x <= r.x + r.w && r.y <= d.y + d.h && d.y <= r.y + r.h) {
            int x0 = MIN(r.x, d.x), y0 = MIN(r.y, d.y);
            int x1 = MAX(r.x + r.w, d.x + d.w), y1 = MAX(r.y + r.h, d.y + d.h);
            r = rect(x0, y0, x1 - x0, y1 - y0);
            wm->damage[i] = wm->damage[--wm->ndamage];
            i = 0;
        } else {
            i++;
        }
    }
    if (wm->ndamage == WM_DAMAGE_MAX) {
        for (int i = 0; i < wm->ndamage; i++) {
            wm_rect_t d = wm->damage[i];
            int x0 = MIN(r.x, d.x), y0 = MIN(r.y, d.y);
            int x1 = MAX(r.x + r.w, d.x + d.w), y1 = MAX(r.y + r.h, d.y + d.h);
            r = rect(x0, y0, x1 - x0, y1 - y0);
        }
        wm->ndamage = 0;
    }
    wm->damage[wm->ndamage++] = r;
}

static void damage_rect(wm_t* wm, wm_rect_t r) { wm_damage(wm, r.x, r.y, r.w, r.h); }

void wm_damage_content(wm_t* wm, int win, int x, int y, int w, int h) {
    if (win < 0 || win >= WM_MAX || !wm->win[win].used) return;
    wm_rect_t r = clip(rect(x, y, w, h), rect(0, 0, wm->win[win].w, wm->win[win].h));
    if (r.w) wm_damage(wm, wm->win[win].x + r.x, wm->win[win].y + r.y, r.w, r.h);
}

int wm_take_damage(wm_t* wm, wm_rect_t* out) {
    int n = wm->ndamage;
    for (int i = 0; i < n; i++) out[i] = wm->damage[i];
    wm->ndamage = 0;
    return n;
}

static void damage_bar(wm_t* wm) { wm_damage(wm, 0, 0, wm->W, WM_BAR_H); }

// ---- Windows ----

void wm_init(wm_t* wm, int W, int H, uint32_t* frame, const uint8_t* font) {
    uint8_t* p = (uint8_t*)wm;
    for (unsigned i = 0; i < sizeof *wm; i++) p[i] = 0;
    wm->W = W;
    wm->H = H;
    wm->frame = frame;
    wm->font = font;
    wm->focus = -1;
    wm->menu_hover = -1;
    wm->desk_top = WM_DESK_TOP;
    wm->desk_bottom = WM_DESK_BOTTOM;
    wm_damage(wm, 0, 0, W, H);
}

int wm_add(wm_t* wm, int w, int h, const uint32_t* pixels, const char* title, int tag) {
    int fw = w + 2 * WM_BORDER, fh = h + WM_TITLE_H + 2 * WM_BORDER;
    if (w <= 0 || h <= 0 || fw > wm->W || fh > wm->H - WM_BAR_H) return -1;
    int win = -1;
    for (int i = 0; i < WM_MAX && win < 0; i++)
        if (!wm->win[i].used) win = i;
    if (win < 0) return -1;
    // The cascade: down and right a title bar's height each time, back
    // to the top corner when the window would run off the screen.
    int step = WM_TITLE_H + 4;
    int fx = 32 + (wm->place % 12) * step, fy = WM_BAR_H + 16 + (wm->place % 12) * step;
    if (fx + fw > wm->W || fy + fh > wm->H) {
        wm->place = 0;
        fx = MAX(0, MIN(32, wm->W - fw));
        fy = WM_BAR_H + MAX(0, MIN(16, wm->H - WM_BAR_H - fh));
    }
    wm->place++;
    wm_win_t* x = &wm->win[win];
    x->used = 1;
    x->x = fx + WM_BORDER;
    x->y = fy + WM_BORDER + WM_TITLE_H;
    x->w = w;
    x->h = h;
    x->pixels = pixels;
    x->tag = tag;
    scopy(x->title, title ? title : "", WM_TITLE_MAX);
    wm->order[wm->n++] = win;
    wm_raise(wm, win);
    damage_rect(wm, outer(wm, win));
    return win;
}

static int depth(const wm_t* wm, int win) {
    for (int i = 0; i < wm->n; i++)
        if (wm->order[i] == win) return i;
    return -1;
}

static int valid(const wm_t* wm, int win) { return win >= 0 && win < WM_MAX && wm->win[win].used; }

void wm_remove(wm_t* wm, int win) {
    if (!valid(wm, win)) return;
    damage_rect(wm, outer(wm, win));
    int d = depth(wm, win);
    for (int i = d; i < wm->n - 1; i++) wm->order[i] = wm->order[i + 1];
    wm->n--;
    wm->win[win].used = 0;
    if (wm->focus == win) {
        wm->focus = wm_top(wm);
        if (wm->focus >= 0) damage_rect(wm, title_rect(wm, wm->focus));
    }
    damage_bar(wm);
}

void wm_raise(wm_t* wm, int win) {
    if (!valid(wm, win)) return;
    int d = depth(wm, win);
    if (d != wm->n - 1) {
        for (int i = d; i < wm->n - 1; i++) wm->order[i] = wm->order[i + 1];
        wm->order[wm->n - 1] = win;
        damage_rect(wm, outer(wm, win));
    }
    if (wm->focus != win) {
        if (valid(wm, wm->focus)) damage_rect(wm, title_rect(wm, wm->focus));
        wm->focus = win;
        damage_rect(wm, title_rect(wm, win));
        damage_bar(wm);
    }
}

void wm_move(wm_t* wm, int win, int x, int y) {
    if (!valid(wm, win)) return;
    wm_rect_t f = wm_frame(wm, win);
    int keep = 2 * WM_TITLE_H;
    x = MAX(keep - f.w, MIN(wm->W - keep, x));
    y = MAX(WM_BAR_H, MIN(wm->H - WM_TITLE_H, y));
    if (x == f.x && y == f.y) return;
    damage_rect(wm, outer(wm, win));
    wm->win[win].x += x - f.x;
    wm->win[win].y += y - f.y;
    damage_rect(wm, outer(wm, win));
}

void wm_set_title(wm_t* wm, int win, const char* title) {
    if (!valid(wm, win)) return;
    scopy(wm->win[win].title, title, WM_TITLE_MAX);
    damage_rect(wm, title_rect(wm, win));
    damage_bar(wm);
}

void wm_set_pixels(wm_t* wm, int win, const uint32_t* pixels) {
    if (!valid(wm, win)) return;
    wm->win[win].pixels = pixels;
    damage_rect(wm, content_rect(wm, win));
}

void wm_set_clock(wm_t* wm, const char* text) {
    // The old text's place too, in case the new one is shorter.
    wm_damage(wm, clock_x(wm), 0, wm->W - clock_x(wm), WM_BAR_H);
    scopy(wm->clock, text, sizeof wm->clock);
    wm_damage(wm, clock_x(wm), 0, wm->W - clock_x(wm), WM_BAR_H);
}

void wm_set_desktop(wm_t* wm, uint32_t top, uint32_t bottom) {
    wm->desk_top = top & 0xFFFFFF;
    wm->desk_bottom = bottom & 0xFFFFFF;
    wm_damage(wm, 0, WM_BAR_H, wm->W, wm->H - WM_BAR_H);
}

// ---- The menu ----

void wm_menu_open(wm_t* wm, int x, int y, const char* const* items, int n) {
    if (wm->menu_open) wm_menu_close(wm);
    n = MAX(0, MIN(WM_MENU_MAX, n));
    for (int i = 0; i < n; i++) scopy(wm->menu_items[i], items[i], sizeof wm->menu_items[i]);
    wm->menu_n = n;
    int h = n * WM_ITEM_H + 4;
    wm->menu_x = MAX(0, MIN(wm->W - WM_MENU_W, x));
    wm->menu_y = MAX(0, MIN(wm->H - h, y));
    wm->menu_open = 1;
    wm->menu_hover = -1;
    damage_rect(wm, wm_menu_rect(wm));
}

void wm_menu_close(wm_t* wm) {
    if (!wm->menu_open) return;
    damage_rect(wm, wm_menu_rect(wm));
    wm->menu_open = 0;
    wm->menu_hover = -1;
}

void wm_menu_hover(wm_t* wm, int item) {
    if (!wm->menu_open || item == wm->menu_hover) return;
    if (item < -1 || item >= wm->menu_n) item = -1;
    wm->menu_hover = item;
    damage_rect(wm, wm_menu_rect(wm));
}

// ---- What is where ----

wm_hit_t wm_hit(const wm_t* wm, int x, int y) {
    wm_hit_t h = { WM_DESKTOP, -1, 0, 0, -1 };
    if (wm->menu_open && inside(wm_menu_rect(wm), x, y)) {
        int i = (y - wm->menu_y - 2) / WM_ITEM_H;
        if (y >= wm->menu_y + 2 && i < wm->menu_n) h.part = WM_MENU_ITEM, h.item = i;
        else h.part = WM_MENU_GAP;
        return h;
    }
    if (y >= 0 && y < WM_BAR_H) {
        h.part = WM_BAR;
        if (inside(menu_button(), x, y)) h.part = WM_BAR_MENU;
        int slot = 0, slots = bar_slots(wm);
        for (int i = 0; i < WM_MAX && slot < slots; i++) {
            if (!wm->win[i].used) continue;
            if (inside(bar_button(slot), x, y)) h.part = WM_BAR_WIN, h.win = i;
            slot++;
        }
        return h;
    }
    for (int d = wm->n - 1; d >= 0; d--) {
        int win = wm->order[d];
        if (!inside(wm_frame(wm, win), x, y)) continue;
        h.win = win;
        if (inside(content_rect(wm, win), x, y)) {
            h.part = WM_CONTENT;
            h.x = x - wm->win[win].x;
            h.y = y - wm->win[win].y;
        } else if (inside(close_rect(wm, win), x, y)) {
            h.part = WM_CLOSE;
        } else if (inside(title_rect(wm, win), x, y)) {
            h.part = WM_TITLE;
        } else {
            h.part = WM_FRAME;
        }
        return h;
    }
    return h;
}

// ---- Drawing, clipped to c ----

uint32_t wm_desktop(const wm_t* wm, int y) {
    // A vertical gradient, each channel on its own.
    int t = (y - WM_BAR_H) * 256 / MAX(1, wm->H - WM_BAR_H);
    t = MAX(0, MIN(256, t));
    uint32_t c = 0;
    for (int s = 0; s < 24; s += 8) {
        int a = (int)(wm->desk_top >> s & 255), b = (int)(wm->desk_bottom >> s & 255);
        c |= (uint32_t)(a + (b - a) * t / 256) << s;
    }
    return c;
}

static void fill(wm_t* wm, wm_rect_t c, wm_rect_t r, uint32_t color) {
    r = clip(r, c);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t* row = wm->frame + (long)y * wm->W;
        for (int x = r.x; x < r.x + r.w; x++) row[x] = color;
    }
}

// Mixes color into what is there, a/256 of it.
static void blend(wm_t* wm, wm_rect_t c, wm_rect_t r, uint32_t color, int a) {
    r = clip(r, c);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t* row = wm->frame + (long)y * wm->W;
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t d = row[x], out = 0;
            for (int s = 0; s < 24; s += 8) {
                int dc = (int)(d >> s & 255), cc = (int)(color >> s & 255);
                out |= (uint32_t)(dc + (cc - dc) * a / 256) << s;
            }
            row[x] = out;
        }
    }
}

// Text, as many whole characters as fit in room pixels.
static void text(wm_t* wm, wm_rect_t c, int x, int y, const char* s, uint32_t color, int room) {
    for (int i = 0; s[i] && (i + 1) * 8 <= room; i++) {
        const uint8_t* g = wm->font + (uint8_t)s[i] * 16;
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if ((g[row] & (0x80 >> col)) && inside(c, x + i * 8 + col, y + row))
                    wm->frame[(long)(y + row) * wm->W + x + i * 8 + col] = color;
    }
}

// Text in the middle of r, or from its left edge if it does not fit.
static void centred(wm_t* wm, wm_rect_t c, wm_rect_t r, const char* s, uint32_t color) {
    int w = slen(s) * 8;
    int x = w <= r.w - 8 ? r.x + (r.w - w) / 2 : r.x + 4;
    text(wm, c, x, r.y + (r.h - 16) / 2, s, color, r.x + r.w - 4 - x);
}

static void draw_bar(wm_t* wm, wm_rect_t c) {
    fill(wm, c, rect(0, 0, wm->W, WM_BAR_H - 1), WM_C_BAR);
    fill(wm, c, rect(0, WM_BAR_H - 1, wm->W, 1), 0x9AA2AE);
    fill(wm, c, menu_button(), WM_C_TITLE);
    centred(wm, c, menu_button(), "RingTail", WM_C_WHITE);
    int slot = 0, slots = bar_slots(wm);
    for (int i = 0; i < WM_MAX && slot < slots; i++) {
        if (!wm->win[i].used) continue;
        wm_rect_t b = bar_button(slot++);
        int on = i == wm->focus;
        fill(wm, c, b, on ? WM_C_TITLE : 0xB8BEC8);
        centred(wm, c, b, wm->win[i].title, on ? WM_C_WHITE : WM_C_TEXT);
    }
    text(wm, c, clock_x(wm), (WM_BAR_H - 16) / 2, wm->clock, WM_C_TEXT, wm->W - clock_x(wm));
}

static void draw_window(wm_t* wm, wm_rect_t c, int win) {
    wm_win_t* w = &wm->win[win];
    wm_rect_t f = wm_frame(wm, win);
    // The shadow, under the frame, shows to the right and below.
    blend(wm, c, rect(f.x + WM_SHADOW, f.y + f.h, f.w, WM_SHADOW), 0, 90);
    blend(wm, c, rect(f.x + f.w, f.y + WM_SHADOW, WM_SHADOW, f.h - WM_SHADOW), 0, 90);
    fill(wm, c, f, WM_C_BORDER);
    wm_rect_t t = title_rect(wm, win);
    fill(wm, c, t, win == wm->focus ? WM_C_TITLE : WM_C_TITLE_OFF);
    wm_rect_t x = close_rect(wm, win);
    text(wm, c, t.x + 8, t.y + (WM_TITLE_H - 16) / 2, w->title, WM_C_WHITE, x.x - 8 - (t.x + 8));
    fill(wm, c, x, WM_C_CLOSE);
    text(wm, c, x.x + (WM_CLOSE_W - 8) / 2, x.y + (WM_CLOSE_W - 16) / 2, "x", WM_C_WHITE, 8);
    wm_rect_t in = clip(content_rect(wm, win), c);
    if (!w->pixels) {
        fill(wm, c, in, WM_C_BLANK);
        return;
    }
    for (int y = in.y; y < in.y + in.h; y++) {
        const uint32_t* src = w->pixels + (long)(y - w->y) * w->w + (in.x - w->x);
        uint32_t* dst = wm->frame + (long)y * wm->W + in.x;
        for (int i = 0; i < in.w; i++) dst[i] = src[i];
    }
}

static void draw_menu(wm_t* wm, wm_rect_t c) {
    wm_rect_t m = wm_menu_rect(wm);
    blend(wm, c, rect(m.x + 4, m.y + m.h, m.w, 4), 0, 90);
    blend(wm, c, rect(m.x + m.w, m.y + 4, 4, m.h - 4), 0, 90);
    fill(wm, c, m, WM_C_BORDER);
    fill(wm, c, rect(m.x + 1, m.y + 1, m.w - 2, m.h - 2), WM_C_MENU);
    for (int i = 0; i < wm->menu_n; i++) {
        wm_rect_t it = rect(m.x + 2, m.y + 2 + i * WM_ITEM_H, m.w - 4, WM_ITEM_H);
        if (i == wm->menu_hover) fill(wm, c, it, WM_C_HOVER);
        text(wm, c, it.x + 10, it.y + (WM_ITEM_H - 16) / 2, wm->menu_items[i], WM_C_TEXT, it.w - 14);
    }
}

void wm_compose(wm_t* wm, wm_rect_t r) {
    wm_rect_t c = clip(r, rect(0, 0, wm->W, wm->H));
    if (!c.w) return;
    for (int y = MAX(c.y, WM_BAR_H); y < c.y + c.h; y++) fill(wm, c, rect(c.x, y, c.w, 1), wm_desktop(wm, y));
    if (c.y < WM_BAR_H) draw_bar(wm, c);
    for (int d = 0; d < wm->n; d++)
        if (clip(outer(wm, wm->order[d]), c).w) draw_window(wm, c, wm->order[d]);
    if (wm->menu_open) {
        wm_rect_t m = wm_menu_rect(wm);
        m.w += 4;
        m.h += 4;
        if (clip(m, c).w) draw_menu(wm, c);
    }
}

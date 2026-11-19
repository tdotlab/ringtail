// Host test for the window manager's core (src/programs/lib/wm.c):
// placing, raising, moving and removing windows, what is under a point,
// the damage list, and composing the screen, checked pixel by pixel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wm.h"

static int fails, checks;

static void check(int ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

#define W 640
#define H 480
static uint32_t frame[W * H];
static uint8_t font[256 * 16];  // blank but for 'W', which is solid
static wm_t wm;

static uint32_t at(int x, int y) { return frame[y * W + x]; }

static uint32_t shaded(uint32_t d) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        int dc = (int)(d >> s & 255);
        r |= (uint32_t)(dc - dc * 90 / 256) << s;
    }
    return r;
}

static void compose_all(void) {
    wm_rect_t all = { 0, 0, W, H };
    wm_compose(&wm, all);
}

// Whether some waiting damage covers the point; takes the damage.
static wm_rect_t taken[WM_DAMAGE_MAX];
static int ntaken;
static void take(void) { ntaken = wm_take_damage(&wm, taken); }
static int covered(int x, int y) {
    for (int i = 0; i < ntaken; i++)
        if (x >= taken[i].x && x < taken[i].x + taken[i].w && y >= taken[i].y && y < taken[i].y + taken[i].h)
            return 1;
    return 0;
}

static int part_at(int x, int y, int* win) {
    wm_hit_t h = wm_hit(&wm, x, y);
    if (win) *win = h.win;
    return h.part;
}

int main(void) {
    memset(font + 'W' * 16, 0xFF, 16);
    static uint32_t pa[100 * 50], pb[120 * 80];
    for (int i = 0; i < 100 * 50; i++) pa[i] = 0x010000u + (uint32_t)i;
    for (int i = 0; i < 120 * 80; i++) pb[i] = 0x800000u + (uint32_t)i;

    // ---- An empty screen ----
    wm_init(&wm, W, H, frame, font);
    take();
    check(ntaken == 1 && taken[0].x == 0 && taken[0].y == 0 && taken[0].w == W && taken[0].h == H,
          "it starts with the whole screen to draw");
    compose_all();
    check(at(W / 2, 5) == WM_C_BAR && at(W / 2, WM_BAR_H - 1) == 0x9AA2AE, "the bar, and its bottom line");
    check(at(10, 10) == WM_C_TITLE, "the menu button");
    check(at(3, H - 1) == wm_desktop(&wm, H - 1) && at(W - 1, WM_BAR_H) == wm_desktop(&wm, WM_BAR_H),
          "the desktop below it");
    check(wm_desktop(&wm, WM_BAR_H) != wm_desktop(&wm, H - 1), "is a gradient");
    check(wm_top(&wm) == -1 && wm.focus == -1, "no windows, no focus");
    check(part_at(W / 2, H / 2, 0) == WM_DESKTOP && part_at(W / 2, 5, 0) == WM_BAR && part_at(10, 10, 0) == WM_BAR_MENU,
          "hits on the desktop, the bar, the menu button");

    // ---- One window ----
    int a = wm_add(&wm, 100, 50, pa, "aaa", 7);
    check(a == 0 && wm.win[a].tag == 7 && wm.focus == a && wm_top(&wm) == a, "add a window: on top and focused");
    wm_rect_t fa = wm_frame(&wm, a);
    check(fa.w == 102 && fa.h == 50 + WM_TITLE_H + 2 && fa.y >= WM_BAR_H && fa.x >= 0 && fa.x + fa.w <= W,
          "its frame is the content, a title bar and a border, on the screen below the bar");
    take();
    check(covered(fa.x, fa.y) && covered(fa.x + fa.w + WM_SHADOW - 1, fa.y + fa.h + WM_SHADOW - 1),
          "adding it damages it and its shadow");
    check(covered(W / 2, 5), "and the bar, which gets a button for it");
    compose_all();
    int ax = wm.win[a].x, ay = wm.win[a].y;
    check(at(ax, ay) == pa[0] && at(ax + 99, ay + 49) == pa[100 * 50 - 1] && at(ax + 37, ay + 11) == pa[11 * 100 + 37],
          "its pixels are on the screen");
    check(at(fa.x, fa.y) == WM_C_BORDER && at(fa.x + fa.w - 1, fa.y + fa.h - 1) == WM_C_BORDER, "inside a border");
    check(at(fa.x + 3, fa.y + 3) == WM_C_TITLE, "with a focused title bar");
    int cx = fa.x + fa.w - 1 - 3 - WM_CLOSE_W, cy = fa.y + 1 + (WM_TITLE_H - WM_CLOSE_W) / 2;
    check(at(cx + 1, cy + 1) == WM_C_CLOSE && at(cx + WM_CLOSE_W - 1, cy + WM_CLOSE_W - 1) == WM_C_CLOSE,
          "and a close button");
    check(at(fa.x + fa.w + 1, fa.y + 20) == shaded(wm_desktop(&wm, fa.y + 20)) &&
          at(fa.x + 20, fa.y + fa.h + 1) == shaded(wm_desktop(&wm, fa.y + fa.h + 1)),
          "its shadow darkens the desktop right and below");
    check(at(fa.x + 2, fa.y + fa.h + 1) == wm_desktop(&wm, fa.y + fa.h + 1) &&
          at(fa.x + fa.w + 1, fa.y + 2) == wm_desktop(&wm, fa.y + 2), "offset, so the corners stay clear");
    check(at(WM_BTN_X + 2, 5) == WM_C_TITLE, "its button on the bar shows it focused");

    int w;
    wm_hit_t h = wm_hit(&wm, ax + 5, ay + 7);
    check(h.part == WM_CONTENT && h.win == a && h.x == 5 && h.y == 7, "a hit in the content, in its coordinates");
    h = wm_hit(&wm, ax + 99, ay + 49);
    check(h.part == WM_CONTENT && h.x == 99 && h.y == 49, "to its last pixel");
    check(part_at(ax + 100, ay + 10, &w) == WM_FRAME && w == a && part_at(fa.x, fa.y, 0) == WM_FRAME,
          "the border is the frame");
    check(part_at(fa.x + 5, fa.y + 5, &w) == WM_TITLE && w == a, "the title bar");
    check(part_at(cx, cy, &w) == WM_CLOSE && part_at(cx + WM_CLOSE_W - 1, cy + WM_CLOSE_W - 1, 0) == WM_CLOSE &&
          part_at(cx - 1, cy, 0) == WM_TITLE, "the close button, exactly");
    check(part_at(fa.x + fa.w, fa.y + 20, 0) == WM_DESKTOP && part_at(fa.x + fa.w + 1, fa.y + 20, 0) == WM_DESKTOP,
          "the shadow is not the window");
    check(part_at(WM_BTN_X, 5, &w) == WM_BAR_WIN && w == a && part_at(WM_BTN_X + WM_BTN_W, 5, 0) == WM_BAR,
          "its bar button");

    // ---- A second ----
    int b = wm_add(&wm, 120, 80, pb, "bbb", 8);
    wm_rect_t fb = wm_frame(&wm, b);
    check(b == 1 && wm.focus == b && wm_top(&wm) == b, "a second window takes the focus");
    check(fb.x > fa.x && fb.y > fa.y && fb.x < fa.x + fa.w && fb.y < fa.y + fa.h, "cascaded over the first");
    compose_all();
    check(at(fa.x + 3, fa.y + 3) == WM_C_TITLE_OFF && at(fb.x + 3, fb.y + 3) == WM_C_TITLE,
          "the first's title bar goes grey");
    int ox = fb.x + 10, oy = wm.win[b].y + 5;  // in both
    check(part_at(ox, oy, &w) == WM_CONTENT && w == b && at(ox, oy) == pb[(oy - wm.win[b].y) * 120 + (ox - wm.win[b].x)],
          "the top one is hit, and drawn, where they overlap");
    check(part_at(WM_BTN_X + WM_BTN_W + 4, 5, &w) == WM_BAR_WIN && w == b && at(WM_BTN_X + 2, 5) != WM_C_TITLE &&
          at(WM_BTN_X + WM_BTN_W + 6, 5) == WM_C_TITLE, "the bar has a button for each, the focused one lit");

    take();
    wm_raise(&wm, a);
    check(wm_top(&wm) == a && wm.focus == a && part_at(ox, oy, &w) == WM_CONTENT && w == a,
          "raising the first puts it on top");
    take();
    check(covered(ax + 1, ay + 49) && covered(fb.x + 115, fb.y + 3) && covered(WM_BTN_X, 5),
          "and damages it, the other's title and the bar");
    compose_all();
    check(at(ox, oy) == pa[(oy - ay) * 100 + (ox - ax)], "and draws it over");
    wm_raise(&wm, a);
    take();
    check(ntaken == 0, "raising what is on top and focused changes nothing");

    // ---- A compose touches only its rectangle ----
    for (int i = 0; i < W * H; i++) frame[i] = 0xABCDEF;
    wm_rect_t r = { 50, 60, 7, 9 };
    wm_compose(&wm, r);
    int in = 1, out = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int inr = x >= 50 && x < 57 && y >= 60 && y < 69;
            if (inr) in &= at(x, y) != 0xABCDEF;
            else out &= at(x, y) == 0xABCDEF;
        }
    check(in && out, "a compose draws its rectangle and nothing else");
    wm_rect_t part = { ax + 10, ay + 5, 5, 5 };
    wm_compose(&wm, part);
    check(at(ax + 12, ay + 6) == pa[6 * 100 + 12] && at(ax + 10, ay + 5) == pa[5 * 100 + 10],
          "part of a window's content is the right part");
    wm_rect_t sh = { fa.x + fa.w + 1, fa.y + 20, 2, 2 };
    wm_compose(&wm, sh);
    check(at(fa.x + fa.w + 1, fa.y + 20) == shaded(wm_desktop(&wm, fa.y + 20)), "and a shadow alone is drawn");
    for (int i = 0; i < W * H; i++) frame[i] = 0xABCDEF;
    wm_rect_t whole = { fa.x - 3, fa.y - 3, fa.w + 20, fa.h + 20 };
    wm_compose(&wm, whole);
    out = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!(x >= whole.x && x < whole.x + whole.w && y >= whole.y && y < whole.y + whole.h))
                out &= at(x, y) == 0xABCDEF;
    check(out && at(ax, ay) == pa[0], "even with windows and shadows in it");
    for (int i = 0; i < W * H; i++) frame[i] = 0xABCDEF;
    wm_rect_t edge = { -5, -6, 10, 10 }, off = { W, 0, 10, 10 }, neg = { 5, 5, -3, 4 };
    wm_compose(&wm, edge);
    wm_compose(&wm, off);
    wm_compose(&wm, neg);
    out = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!(x < 5 && y < 4)) out &= at(x, y) == 0xABCDEF;
    check(out && at(0, 0) != 0xABCDEF && at(4, 3) != 0xABCDEF, "rectangles off the screen are clipped to it");
    compose_all();

    // ---- Damage ----
    take();
    wm_damage(&wm, 10, 40, 5, 5);
    wm_damage(&wm, 15, 40, 5, 5);
    take();
    check(ntaken == 1 && taken[0].x == 10 && taken[0].w == 10 && taken[0].h == 5, "touching rectangles merge");
    wm_damage(&wm, 10, 40, 5, 5);
    wm_damage(&wm, 100, 100, 5, 5);
    wm_damage(&wm, 200, 200, 5, 5);
    take();
    check(ntaken == 3, "apart they stay apart");
    wm_damage(&wm, 10, 40, 5, 5);
    wm_damage(&wm, 100, 100, 5, 5);
    wm_damage(&wm, 13, 43, 90, 60);
    take();
    check(ntaken == 1 && taken[0].x == 10 && taken[0].y == 40 && taken[0].w == 95 && taken[0].h == 65,
          "one that bridges two merges all three");
    for (int i = 0; i < WM_DAMAGE_MAX + 3; i++) wm_damage(&wm, 10 + i * 20, 40 + i * 30, 3, 3);
    take();
    int all = ntaken <= WM_DAMAGE_MAX;
    for (int i = 0; i < WM_DAMAGE_MAX + 3; i++) all &= covered(10 + i * 20, 40 + i * 30) && covered(12 + i * 20, 42 + i * 30);
    check(all, "past WM_DAMAGE_MAX they still cover everything");
    wm_damage(&wm, -10, -10, 5, 5);
    wm_damage(&wm, W, 3, 5, 5);
    wm_damage(&wm, 3, 3, 0, 5);
    wm_damage(&wm, 3, 3, 5, -1);
    take();
    check(ntaken == 0, "damage off the screen or empty is nothing");
    wm_damage(&wm, W - 2, H - 3, 10, 10);
    take();
    check(ntaken == 1 && taken[0].x == W - 2 && taken[0].y == H - 3 && taken[0].w == 2 && taken[0].h == 3,
          "and is clipped to it");
    wm_damage_content(&wm, a, -5, -5, 10, 10);
    take();
    check(ntaken == 1 && taken[0].x == ax && taken[0].y == ay && taken[0].w == 5 && taken[0].h == 5,
          "content damage is clipped to the content and moved onto the screen");
    wm_damage_content(&wm, a, 3, 7, 10, 10);
    take();
    check(ntaken == 1 && taken[0].x == ax + 3 && taken[0].y == ay + 7 && taken[0].w == 10 && taken[0].h == 10,
          "inside it, it is where it says");
    wm_damage_content(&wm, a, 100, 0, 5, 5);
    wm_damage_content(&wm, 5, 0, 0, 5, 5);
    wm_damage_content(&wm, -1, 0, 0, 5, 5);
    wm_damage_content(&wm, WM_MAX, 0, 0, 5, 5);
    take();
    check(ntaken == 0, "content damage outside it, or for no window, is nothing");

    // ---- Moving ----
    wm_move(&wm, a, 200, 150);
    fa = wm_frame(&wm, a);
    check(fa.x == 200 && fa.y == 150 && wm.win[a].x == 201 && wm.win[a].y == 151 + WM_TITLE_H, "move a window");
    wm_rect_t old = wm_frame(&wm, b);
    take();
    wm_move(&wm, b, 300, 200);
    take();
    check(covered(old.x, old.y) && covered(old.x + old.w + WM_SHADOW - 1, old.y + old.h + WM_SHADOW - 1) &&
          covered(300, 200) && covered(300 + old.w + WM_SHADOW - 1, 200 + old.h + WM_SHADOW - 1),
          "moving damages where it was and where it is, shadows too");
    wm_move(&wm, b, 300, 200);
    take();
    check(ntaken == 0, "moving to where it is does nothing");
    wm_move(&wm, a, -1000, -1000);
    fa = wm_frame(&wm, a);
    check(fa.x == 2 * WM_TITLE_H - fa.w && fa.y == WM_BAR_H, "a window cannot go off the left or under the bar");
    wm_move(&wm, a, 5000, 5000);
    fa = wm_frame(&wm, a);
    check(fa.x == W - 2 * WM_TITLE_H && fa.y == H - WM_TITLE_H, "nor off the right or the bottom");
    compose_all();
    check(at(fa.x + 2, fa.y + 2) == (wm.focus == a ? WM_C_TITLE : WM_C_TITLE_OFF) &&
          part_at(fa.x + 2, fa.y + 2, &w) == WM_TITLE && w == a, "its title bar is still there to grab");
    wm_move(&wm, a, 200, 150);

    // ---- Titles, pixels, the clock ----
    char lng[WM_TITLE_MAX + 20];
    memset(lng, 'W', sizeof lng - 1);
    lng[sizeof lng - 1] = 0;
    int n = wm_add(&wm, 60, 30, 0, lng, 0);
    fa = wm_frame(&wm, n);
    check(n == 2 && strlen(wm.win[n].title) == WM_TITLE_MAX - 1, "a long title is cut to WM_TITLE_MAX");
    compose_all();
    check(at(wm.win[n].x, wm.win[n].y) == WM_C_BLANK && at(wm.win[n].x + 59, wm.win[n].y + 29) == WM_C_BLANK,
          "a window without pixels is blank");
    cx = fa.x + fa.w - 1 - 3 - WM_CLOSE_W;
    check(at(fa.x + 9, fa.y + 10) == WM_C_WHITE, "the title is drawn");
    int clean = 1;
    for (int x = cx - 8; x < cx + WM_CLOSE_W; x++)
        for (int y = fa.y + 1; y < fa.y + 1 + WM_TITLE_H; y++)
            clean &= at(x, y) != WM_C_WHITE || (x >= cx + 5 && x < cx + 13);
    check(clean, "and stops short of the close button");
    int tx = fa.x + 1 + 8, room = cx - 8 - tx;
    clean = room % 8 != 0 && at(tx + room / 8 * 8 - 1, fa.y + 10) == WM_C_WHITE;
    for (int x = tx + room / 8 * 8; x < tx + room; x++) clean &= at(x, fa.y + 10) != WM_C_WHITE;
    check(clean, "in whole characters");
    check(at(fa.x + fa.w - 1, fa.y + 10) == WM_C_BORDER, "nor runs over the border");
    wm_set_pixels(&wm, n, pa);
    take();
    check(covered(wm.win[n].x, wm.win[n].y) && covered(wm.win[n].x + 59, wm.win[n].y + 29), "new pixels damage the content");
    compose_all();
    check(at(wm.win[n].x + 1, wm.win[n].y) == pa[1], "and show");
    wm_set_title(&wm, n, "W");
    take();
    check(covered(fa.x + 5, fa.y + 5) && covered(WM_BTN_X + 2 * (WM_BTN_W + 4) + 1, 5), "a new title damages the title bar and the bar");
    compose_all();
    check(at(fa.x + 9, fa.y + 10) == WM_C_WHITE && at(fa.x + 9 + 8, fa.y + 10) == WM_C_TITLE, "and shows");
    wm_set_clock(&wm, "WWW");
    take();
    compose_all();
    check(covered(W - 8 - 24, 6) && at(W - 8 - 24, 6) == WM_C_TEXT && at(W - 9, 21) == WM_C_TEXT && at(W - 8, 10) == WM_C_BAR,
          "the clock goes at the right of the bar");
    wm_set_clock(&wm, "W");
    take();
    compose_all();
    check(covered(W - 8 - 24, 6) && at(W - 8 - 24, 6) == WM_C_BAR, "a shorter time clears the old one");

    // ---- Removing ----
    wm_remove(&wm, n);
    wm_raise(&wm, b);
    wm_rect_t gone = wm_frame(&wm, b);
    take();
    wm_remove(&wm, b);
    check(wm.focus == a && wm_top(&wm) == a && wm.n == 1, "removing the focused window focuses the one on top");
    take();
    check(covered(gone.x, gone.y) && covered(gone.x + gone.w + WM_SHADOW - 1, gone.y + gone.h + WM_SHADOW - 1) &&
          covered(WM_BTN_X + WM_BTN_W + 10, 5), "and damages where it was, and the bar");
    compose_all();
    check(part_at(gone.x + 5, gone.y + 5, 0) == WM_DESKTOP && at(gone.x + gone.w - 3, gone.y + gone.h - 3) ==
          wm_desktop(&wm, gone.y + gone.h - 3), "the desktop shows where it was");
    check(at(WM_BTN_X + WM_BTN_W + 10, 5) == WM_C_BAR, "its button is gone");
    wm_remove(&wm, b);
    wm_remove(&wm, -1);
    wm_remove(&wm, WM_MAX);
    check(wm.n == 1 && wm.focus == a, "removing a window twice, or none, does nothing");
    wm_remove(&wm, a);
    check(wm.n == 0 && wm.focus == -1 && wm_top(&wm) == -1, "the last one leaves no focus");

    // ---- Sizes and the table ----
    check(wm_add(&wm, 0, 10, 0, "", 0) == -1 && wm_add(&wm, 10, 0, 0, "", 0) == -1 && wm_add(&wm, -5, 10, 0, "", 0) == -1,
          "empty windows are refused");
    check(wm_add(&wm, W - 1, 10, 0, "", 0) == -1 && wm_add(&wm, 10, H - WM_BAR_H - WM_TITLE_H - 1, 0, "", 0) == -1,
          "so are ones the screen cannot hold");
    int big = wm_add(&wm, W - 2, H - WM_BAR_H - WM_TITLE_H - 2, 0, 0, 0);
    fa = wm_frame(&wm, big);
    check(big >= 0 && fa.x == 0 && fa.y == WM_BAR_H && fa.w == W && fa.y + fa.h == H, "the largest fits exactly");
    wm_remove(&wm, big);
    int made = 0, onscreen = 1;
    for (int i = 0; i < WM_MAX + 2; i++) {
        int k = wm_add(&wm, 200, 150, 0, "", i);
        if (k < 0) break;
        made++;
        wm_rect_t f = wm_frame(&wm, k);
        onscreen &= f.x >= 0 && f.y >= WM_BAR_H && f.x + f.w <= W && f.y + f.h <= H;
    }
    check(made == WM_MAX && onscreen, "WM_MAX windows, every one placed on the screen");
    for (int i = 0; i < WM_MAX; i++) wm_remove(&wm, i);
    check(wm.n == 0, "and all removed");

    // ---- The bar fills up ----
    for (int i = 0; i < 6; i++) wm_add(&wm, 50, 50, 0, "", 0);
    int slots = 0;
    for (int x = WM_BTN_X; x < W; x += WM_BTN_W + 4)
        if (part_at(x + 1, 5, &w) == WM_BAR_WIN) slots++;
    check(slots == (W - 8 - 8 - 12 - WM_BTN_X + 4) / (WM_BTN_W + 4) && slots < 6, "the bar shows the buttons that fit");
    compose_all();
    check(at(W - 9, 21) == WM_C_TEXT, "and leaves the clock alone");
    int fit = 1;
    for (int len = 0; len < 22; len++) {
        char ck[24];
        memset(ck, 'W', (size_t)len);
        ck[len] = 0;
        wm_set_clock(&wm, ck);
        int cxk = W - 8 - 8 * len, last = -1, nb = 0;
        for (int x = 0; x < W; x++)
            if (part_at(x, 5, 0) == WM_BAR_WIN) last = x;
        for (int x = WM_BTN_X; x < W; x += WM_BTN_W + 4)
            if (part_at(x, 5, 0) == WM_BAR_WIN) nb++;
        fit &= (nb == 0 || last < cxk - 11) && (nb == 6 || WM_BTN_X + (nb + 1) * (WM_BTN_W + 4) - 4 > cxk - 12);
    }
    check(fit, "as many as fit before the clock, however long it is");
    wm_set_clock(&wm, "W");
    for (int i = 0; i < WM_MAX; i++) wm_remove(&wm, i);

    // ---- The menu ----
    static uint32_t white[300 * 200];
    for (int i = 0; i < 300 * 200; i++) white[i] = 0xFFFFFF;
    a = wm_add(&wm, 300, 200, white, "", 0);
    wm_move(&wm, a, 50, 50);
    const char* items[] = { "One", "Two", "Three" };
    wm_menu_open(&wm, 100, 100, items, 3);
    wm_rect_t m = wm_menu_rect(&wm);
    check(m.x == 100 && m.y == 100 && m.w == WM_MENU_W && m.h == 3 * WM_ITEM_H + 4, "open the menu");
    h = wm_hit(&wm, 110, 102);
    check(h.part == WM_MENU_ITEM && h.item == 0, "its first item");
    h = wm_hit(&wm, 110, 102 + 3 * WM_ITEM_H - 1);
    check(h.part == WM_MENU_ITEM && h.item == 2, "its last");
    check(part_at(110, 101, 0) == WM_MENU_GAP && part_at(110, 102 + 3 * WM_ITEM_H, 0) == WM_MENU_GAP,
          "its border is not an item");
    check(part_at(100 + WM_MENU_W, 110, 0) != WM_MENU_ITEM && part_at(99, 110, 0) != WM_MENU_ITEM, "nor beside it");
    for (int i = 0; i < W * H; i++) frame[i] = 0xABCDEF;
    wm_rect_t msh = { 100 + WM_MENU_W + 1, 120, 2, 2 };
    wm_compose(&wm, msh);
    check(at(100 + WM_MENU_W + 1, 120) == shaded(0xFFFFFF) && at(100 + WM_MENU_W + 1, 120) == 0xA6A6A6,
          "the menu's shadow alone is drawn, over a window");
    wm_menu_hover(&wm, 1);
    take();
    compose_all();
    check(covered(110, 110) && at(105, 102 + WM_ITEM_H + 3) == WM_C_HOVER && at(105, 105) == WM_C_MENU,
          "the item under the pointer is lit");
    wm_menu_hover(&wm, 7);
    check(wm.menu_hover == -1, "an item that is not there lights nothing");
    wm_menu_close(&wm);
    take();
    check(covered(110, 110) && part_at(110, 110, 0) != WM_MENU_ITEM && !wm.menu_open, "close it");
    compose_all();
    check(at(105, 105) != WM_C_MENU, "and it is gone from the screen");
    wm_menu_open(&wm, 100, 100, items, 3);
    take();
    wm_menu_open(&wm, 300, 300, items, 3);
    take();
    check(covered(110, 110) && covered(310, 310) && part_at(110, 110, 0) != WM_MENU_ITEM,
          "opening it somewhere else moves it");
    wm_menu_open(&wm, W - 5, H - 5, items, 3);
    m = wm_menu_rect(&wm);
    check(m.x + m.w == W && m.y + m.h == H, "near the corner it is kept on the screen");
    const char* many[WM_MENU_MAX + 3];
    for (int i = 0; i < WM_MENU_MAX + 3; i++) many[i] = "x";
    wm_menu_open(&wm, 0, 0, many, WM_MENU_MAX + 3);
    check(wm.menu_n == WM_MENU_MAX, "it holds WM_MENU_MAX items");
    wm_menu_close(&wm);

    printf("test_wm: %d of %d checks %s\n", checks - fails, checks, fails ? "passed, FAILURES above" : "passed");
    return fails != 0;
}

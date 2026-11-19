// Host test for the toolkit (src/programs/lib/tk.c): buttons, check
// boxes, fields, lists, the menu bar and the dialog, driven with the
// server's events as pleroma sends them, and what they draw checked
// pixel by pixel; and that drawing stays inside what it says it drew.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tk.h"
#include "tty.h"

static int fails, checks;

static void check(int ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

#define W 400
#define H 300
static uint32_t px[W * H];
static uint8_t font[256 * 16];  // blank but for 'W', which is solid, and '~', a line along its top
static tk_t tk;
static tk_event_t ev;
static int got;  // what the last event fed returned

static uint32_t at(int x, int y) { return px[y * W + x]; }

static int mouse(int x, int y, int b) {
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_MOUSE;
    m.mouse.x = x, m.mouse.y = y, m.mouse.buttons = b;
    return got = tk_handle(&tk, &m, &ev);
}

// A press and a release; what the release said.
static int click(int x, int y) {
    mouse(x, y, 1);
    return mouse(x, y, 0);
}

static int key_mods(int code, int mods) {
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_KEY;
    m.key.key = 0x10, m.key.down = 1, m.key.mods = mods, m.key.code = code;
    got = tk_handle(&tk, &m, &ev);
    m.key.down = 0;  // and its release, which says nothing
    tk_event_t e2;
    if (tk_handle(&tk, &m, &e2)) check(0, "a key's release says nothing");
    return got;
}

static int key(int code) { return key_mods(code, 0); }

static void type(const char* s) {
    for (; *s; s++) key(*s);
}

static int is(int type, int id, int a, int b) { return got && ev.type == type && ev.id == id && ev.a == a && ev.b == b; }

static void draw(void) { tk_draw(&tk); }

// Draws with every pixel outside what tk_draw says it drew set to a
// mark first: none of it may change.
static int draw_inside(void) {
    for (int i = 0; i < W * H; i++) px[i] = 0x123456;
    tk_rect_t r = tk_draw(&tk);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if ((x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) && at(x, y) != 0x123456) return 0;
    return 1;
}

static void fresh(void) {
    tk_init(&tk, px, W, H, font);
    tk.now = 1000;
}

static void test_buttons(void) {
    fresh();
    tk_rect_t r = tk_draw(&tk);
    check(r.x == 0 && r.y == 0 && r.w == W && r.h == H, "it starts with the whole window to draw");
    check(at(5, 5) == TK_C_BG && at(W - 1, H - 1) == TK_C_BG, "in its background");
    r = tk_draw(&tk);
    check(r.w == 0, "and then nothing, until something changes");

    int lab = tk_label(&tk, 10, 10, 200, "WW\nW");
    int ok = tk_button(&tk, 10, 50, 80, "WW");
    int no = tk_button(&tk, 100, 50, 80, "No");
    check(lab == 0 && ok == 1 && no == 2, "widgets are numbered in order");
    check(tk.wd[lab].h == 32, "a label is as high as its lines");
    check(tk.focus == ok, "the first that takes keys has them");
    draw();
    check(at(10, 10) == TK_C_TEXT && at(17, 25) == TK_C_TEXT && at(18, 10) == TK_C_TEXT && at(10, 26) == TK_C_TEXT,
          "a label's lines");
    check(at(10, 42) == TK_C_BG, "and nothing under them");
    check(at(11, 51) == TK_C_FOCUS && at(10 + 79, 50 + 23) == TK_C_FOCUS, "the focused button's border");
    check(at(101, 51) == TK_C_FACE && at(100, 50) == TK_C_LINE, "the other's");
    check(at(10 + 40 - 8, 50 + 4) == TK_C_TEXT, "the button's text, in its middle");

    check(!mouse(20, 60, 1) && at(20, 60) == TK_C_FACE, "a press says nothing yet");
    draw();
    check(at(20, 60) == TK_C_DOWN, "but the button is drawn down");
    mouse(20, 60, 1);
    check(mouse(20, 60, 0) && is(TK_EV_CLICK, ok, 0, 0), "the release on it clicks it");
    draw();
    check(at(20, 60) == TK_C_FACE, "and it is up again");

    mouse(110, 60, 1);
    check(tk.focus == no, "a press moves the keys to the button");
    mouse(300, 60, 1);
    draw();
    check(at(110, 60) == TK_C_FACE, "dragged off, it is drawn up");
    mouse(115, 60, 1);
    draw();
    check(at(110, 60) == TK_C_DOWN, "back on, down again");
    mouse(300, 200, 1);
    check(!mouse(300, 200, 0), "let go off it: no click");
    mouse(115, 60, 1);
    check(!mouse(300, 200, 0), "nor when the release is the first it hears of the move");

    check(key('\n') && is(TK_EV_CLICK, no, 0, 0), "Enter clicks the focused button");
    check(key(' ') && is(TK_EV_CLICK, no, 0, 0), "so does Space");
    check(key('x') && is(TK_EV_KEY, -1, 'x', 0), "a key nothing wants is passed on");
    check(key(0x1B) && is(TK_EV_KEY, -1, 0x1B, 0), "Esc too");

    check(!click(5, 200), "a click on nothing says nothing");
    check(tk.focus == no, "and leaves the keys where they were");
}

static void test_focus(void) {
    fresh();
    tk_label(&tk, 0, 0, 50, "a");
    int a = tk_button(&tk, 0, 20, 50, "a");
    int b = tk_check(&tk, 0, 50, 80, "b", 0);
    int c = tk_field(&tk, 0, 80, 80, "");
    int d = tk_list(&tk, 0, 110, 80, 80, TK_ROWS);
    check(tk.focus == a, "the first button has the keys");
    key('\t');
    check(tk.focus == b, "Tab: the check box");
    key('\t');
    key('\t');
    check(tk.focus == d, "then the field, the list");
    key('\t');
    check(tk.focus == a, "and round past the label");
    key_mods('\t', KEY_MOD_SHIFT);
    check(tk.focus == d, "Shift+Tab goes back");
    tk_hide(&tk, d, 1);
    check(tk.focus == -1, "hiding it takes the keys away");
    key('\t');
    check(tk.focus == a, "and Tab skips it");
    key_mods('\t', KEY_MOD_SHIFT);
    check(tk.focus == c, "both ways");
    check(!click(10, 150), "a hidden widget is not there to click");

    // The window losing the keys: the focus is not drawn.
    tk_set_focus(&tk, a);
    draw();
    check(at(1, 21) == TK_C_FOCUS, "the focused button, with the window's keys");
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_FOCUS;
    m.focus.on = 0;
    tk_handle(&tk, &m, &ev);
    draw();
    check(at(1, 21) == TK_C_FACE, "and without them");
}

static void test_check(void) {
    fresh();
    int c = tk_check(&tk, 10, 10, 100, "W", 0);
    draw();
    // The box at 12, 15; the tick's lowest point at its 5th column.
    check(at(12, 15) == TK_C_LINE && at(19, 22) == TK_C_WHITE, "an empty box");
    check(at(34, 14) == TK_C_TEXT, "and its text");
    check(click(15, 20) && is(TK_EV_TOGGLE, c, 1, 0), "a click ticks it");
    draw();
    check(at(19, 22) == TK_C_FOCUS, "and it is drawn ticked");
    check(key(' ') && is(TK_EV_TOGGLE, c, 0, 0), "Space unticks it");
    check(!key('\n') || ev.type == TK_EV_KEY, "Enter is not for it");
    tk_set_on(&tk, c, 1);
    check(tk.wd[c].on == 1, "the program can set it");
}

static void test_field(void) {
    fresh();
    int f = tk_field(&tk, 10, 10, 120, "ab");
    check(tk.wd[f].cur == 2, "a field's cursor starts at the end");
    check(key('c') && is(TK_EV_CHANGE, f, 0, 0) && !strcmp(tk_text(&tk, f), "abc"), "typing adds");
    key(KEY_NAV(NAV_HOME, 0));
    check(!got && tk.wd[f].cur == 0, "Home moves without a change");
    key('x');
    check(!strcmp(tk_text(&tk, f), "xabc"), "typing at the cursor inserts");
    key(KEY_RIGHT);
    key('\b');
    check(!strcmp(tk_text(&tk, f), "xbc") && tk.wd[f].cur == 1, "Backspace takes the one before");
    key(KEY_NAV(NAV_DELETE, 0));
    check(is(TK_EV_CHANGE, f, 0, 0) && !strcmp(tk_text(&tk, f), "xc"), "Delete the one after");
    key(KEY_NAV(NAV_END, 0));
    key(KEY_NAV(NAV_DELETE, 0));
    check(!got && !strcmp(tk_text(&tk, f), "xc"), "Delete at the end does nothing");
    key(KEY_NAV(NAV_HOME, 0));
    key('\b');
    check(!got, "nor Backspace at the start");
    key(KEY_LEFT);
    check(tk.wd[f].cur == 0, "Left stops at the start");
    check(key('\n') && is(TK_EV_SUBMIT, f, 0, 0), "Enter submits");
    check(key_mods('a', KEY_MOD_CTRL) && ev.type == TK_EV_KEY, "Ctrl+key is passed on");

    // Drawing: the text, and the cursor before the character it is at.
    tk_set_text(&tk, f, "WW");
    check(tk.wd[f].cur == 2, "set: the cursor at the end");
    draw();
    check(at(14, 12 + 4) == TK_C_TEXT && at(29, 16) == TK_C_TEXT, "the text");
    check(at(14 + 16 - 1, 16) == TK_C_TEXT && at(14 + 16 + 1, 16) == TK_C_WHITE, "the cursor after it");
    check(at(10, 10) == TK_C_FOCUS, "and the focus's border");

    // A long text scrolls to keep the cursor in sight.
    tk_set_text(&tk, f, "");
    for (int i = 0; i < 40; i++) key('W');
    int room = (120 - 8) / 8 - 1;
    check(tk.wd[f].scroll == 40 - room, "a long text scrolls with the cursor");
    key(KEY_NAV(NAV_HOME, 0));
    check(tk.wd[f].scroll == 0, "and back to the start");
    click(10 + 4 + 3 * 8 + 2, 20);
    check(tk.wd[f].cur == 3, "a click puts the cursor there");
    for (int i = 0; i < TK_TEXT_MAX + 10; i++) key('W');
    check((int)strlen(tk_text(&tk, f)) == TK_TEXT_MAX - 1, "no more than fits");
    check(draw_inside(), "a field draws inside its own box");
}

static tk_item_t items[30];
static char names[30][16];

static void make_items(int n) {
    for (int i = 0; i < n; i++) {
        snprintf(names[i], sizeof names[i], "%c%02d", 'a' + i % 5, i);
        items[i].text = names[i];
        items[i].note = i == 0 ? "WW" : 0;
        items[i].icon = i % 2 ? TK_ICON_FOLDER : TK_ICON_DOC;
    }
}

static void test_rows(void) {
    fresh();
    make_items(30);
    // 10 rows of 20 fit in 202 less its border.
    int l = tk_list(&tk, 10, 10, 200, 202, TK_ROWS);
    tk_list_set(&tk, l, items, 30);
    check(tk.wd[l].sel == 0 && tk.wd[l].top == 0, "a list starts on its first item");
    draw();
    check(at(12, 12) == TK_C_FOCUS, "selected, with the keys");
    check(at(12, 12 + TK_ROW_H) == TK_C_WHITE, "the next not");
    check(at(10 + 200 - TK_SCROLL_W + 3, 12) == TK_C_THUMB, "a scroll bar, its thumb at the top");
    check(at(10 + 200 - TK_SCROLL_W + 3, 200) == TK_C_FACE, "and its track");
    // The note: 16 wide, 6 in from the right of the rows (which end at the bar).
    check(at(11 + 184 - 6 - 16, 15) == TK_C_WHITE && at(11 + 184 - 6 - 18, 15) == TK_C_FOCUS,
          "the first item's note, on the right");

    check(key(KEY_DOWN) && is(TK_EV_SELECT, l, 1, 0), "Down selects the next");
    check(!key(KEY_UP) || is(TK_EV_SELECT, l, 0, 0), "Up the one before");
    check(!key(KEY_UP), "and stops at the first");
    check(key(KEY_LEFT) && ev.type == TK_EV_KEY, "Left is not for rows");
    check(key(KEY_NAV(NAV_END, 0)) && is(TK_EV_SELECT, l, 29, 0) && tk.wd[l].top == 20, "End: the last, scrolled to");
    key(KEY_NAV(NAV_PGUP, 0));
    check(is(TK_EV_SELECT, l, 19, 0) && tk.wd[l].top == 19, "PgUp: a page back");
    key(KEY_NAV(NAV_HOME, 0));
    check(tk.wd[l].sel == 0 && tk.wd[l].top == 0, "Home: the first");
    check(key('c') && is(TK_EV_SELECT, l, 2, 0), "a letter: the next item starting with it");
    check(key('C') && is(TK_EV_SELECT, l, 7, 0), "and the next, whatever its case");
    check(key('\n') && is(TK_EV_OPEN, l, 7, 0), "Enter opens it");

    tk_list_select(&tk, l, 0);
    check(mouse(50, 11 + 3 * TK_ROW_H + 5, 1) && is(TK_EV_SELECT, l, 3, 0), "a press selects");
    mouse(50, 11 + 3 * TK_ROW_H + 5, 0);
    tk.now += TK_DCLICK - 1;
    check(mouse(50, 11 + 3 * TK_ROW_H + 5, 1) && is(TK_EV_OPEN, l, 3, 0), "a second, soon after: open");
    mouse(50, 11 + 3 * TK_ROW_H + 5, 0);
    tk.now += 5;
    check(!mouse(50, 11 + 3 * TK_ROW_H + 5, 1), "a third is a new first: nothing");
    mouse(50, 11 + 3 * TK_ROW_H + 5, 0);
    tk.now += TK_DCLICK;
    check(!mouse(50, 11 + 3 * TK_ROW_H + 5, 1), "too slow is not a double click");
    mouse(50, 11 + 3 * TK_ROW_H + 5, 0);
    tk.now += 1;
    mouse(50, 11 + 4 * TK_ROW_H + 5, 1);
    mouse(50, 11 + 4 * TK_ROW_H + 5, 0);
    tk.now += 1;
    check(mouse(50, 11 + 3 * TK_ROW_H + 5, 1) && is(TK_EV_SELECT, l, 3, 0), "nor is one on another item");
    mouse(50, 11 + 3 * TK_ROW_H + 5, 0);

    // The scroll bar.
    int sx = 10 + 200 - TK_SCROLL_W + 5;
    click(sx, 180);
    check(tk.wd[l].top == 10, "a click on the track below the thumb: a page down");
    check(tk_item_rect(&tk, l, 3).w == 0 && tk_item_rect(&tk, l, 10).y == 11, "items move with it");
    click(sx, 180);
    check(tk.wd[l].top == 20, "and no further than the end");
    click(sx, 15);
    check(tk.wd[l].top == 10, "above it: a page up");
    // The thumb: 200 * 10 / 30 = 66 high, 134 to move in for 20 rows.
    tk_list_select(&tk, l, 0);
    check(tk.wd[l].top == 0, "selecting scrolls to it");
    mouse(sx, 20, 1);
    mouse(sx, 20 + 67, 1);
    check(tk.wd[l].top == 10, "dragging the thumb halfway: halfway down");
    mouse(sx, 300, 1);
    check(tk.wd[l].top == 20, "past the end: the end");
    mouse(sx, 300, 0);
    mouse(sx, 20, 0);
    check(tk.wd[l].top == 20, "let go, it stays");
    check(!click(50, 11 + 9 * TK_ROW_H + 21), "below the last row shown: nothing");

    tk_list_set(&tk, l, items, 10);
    draw();
    check(at(10 + 200 - 5, 100) == TK_C_WHITE && tk_item_rect(&tk, l, 0).w == 198, "ten fit exactly: no scroll bar");
    tk_list_set(&tk, l, items, 3);
    check(tk.wd[l].sel == 0 && tk.wd[l].top == 0, "new items: from the top");
    draw();
    check(at(10 + 200 - 5, 100) == TK_C_WHITE, "no scroll bar when they all fit");
    check(!click(50, 11 + 5 * TK_ROW_H), "below the last item: nothing");
    tk_list_set(&tk, l, items, 0);
    check(tk.wd[l].sel == -1 && !key(KEY_DOWN), "an empty list: nothing selected, nothing to move to");
}

static void test_typeahead(void) {
    fresh();
    make_items(30);  // a00 b01 c02 d03 e04 a05 ...
    int l = tk_list(&tk, 10, 10, 200, 202, TK_ROWS);
    int l2 = tk_list(&tk, 220, 10, 200, 202, TK_ROWS);
    tk_list_set(&tk, l, items, 30);
    tk_list_set(&tk, l2, items, 30);
    tk_set_focus(&tk, l);
    tk_list_select(&tk, l, 7);
    check(key('c') && is(TK_EV_SELECT, l, 12, 0) && key('2') && is(TK_EV_SELECT, l, 22, 0),
          "letters typed together: the first name they begin");
    tk.now += TK_TYPEAHEAD;
    check(key('a') && is(TK_EV_SELECT, l, 25, 0), "after a pause a letter starts again");
    check(!key('z') && tk.wd[l].sel == 25, "and letters no name begins with go nowhere");
    tk.now += TK_TYPEAHEAD;
    key('b');
    tk.now += TK_TYPEAHEAD - 1;
    check(key('1') && is(TK_EV_SELECT, l, 11, 0), "a letter just inside the wait still counts");
    tk.now += TK_TYPEAHEAD;
    check(key('c') && is(TK_EV_SELECT, l, 12, 0) && !key('1') && tk.wd[l].sel == 12,
          "a name that still fits what is typed stays selected");
    check(key('7') && is(TK_EV_SELECT, l, 17, 0), "until it doesn't");
    tk.now += TK_TYPEAHEAD;
    key('d');
    tk_set_focus(&tk, l2);
    check(key('c') && is(TK_EV_SELECT, l2, 2, 0), "letters typed to one list are not carried to another");
    static const tk_item_t mixed[] = { { "Files", 0, 0 }, { "Clock", 0, 0 }, { "cal", 0, 0 } };
    int l3 = tk_list(&tk, 10, 220, 200, 70, TK_ROWS);
    tk_list_set(&tk, l3, mixed, 3);
    tk_set_focus(&tk, l3);
    tk.now += TK_TYPEAHEAD;
    check(key('c') && is(TK_EV_SELECT, l3, 1, 0) && key('a') && is(TK_EV_SELECT, l3, 2, 0),
          "whatever the case of the names");
}

static void test_icons(void) {
    fresh();
    make_items(30);
    // 3 cells across (with 2 for the border, the scroll bar and a little over), 2 rows of 68 in 150.
    int l = tk_list(&tk, 0, 0, 3 * TK_CELL_W + 2 + TK_SCROLL_W + 10, 150, TK_ICONS);
    tk_list_set(&tk, l, items, 30);
    tk_rect_t r = tk_item_rect(&tk, l, 4);
    check(r.x == 1 + TK_CELL_W && r.y == 1 + TK_CELL_H && r.w == TK_CELL_W, "icons: a grid, 3 across");
    check(tk_item_rect(&tk, l, 6).w == 0, "and two rows shown");
    draw();
    // Item 1 is a folder: its body, inside the cell's icon.
    check(at(1 + TK_CELL_W + 44, 1 + 6 + 16) == TK_C_FOLDER, "a folder's icon");
    check(at(1 + 44, 1 + 6 + 16) == TK_C_WHITE, "a page's, under the selection's light frame");
    check(key(KEY_RIGHT) && is(TK_EV_SELECT, l, 1, 0), "Right: the next");
    check(key(KEY_DOWN) && is(TK_EV_SELECT, l, 4, 0), "Down: the one under it");
    key(KEY_DOWN);
    check(is(TK_EV_SELECT, l, 7, 0) && tk.wd[l].top == 1, "and on, scrolling a row");
    key(KEY_NAV(NAV_END, 0));
    key(KEY_DOWN);
    check(!got && tk.wd[l].sel == 29, "Down from the last row stays");
    check(mouse(1 + 2 * TK_CELL_W + 40, 1 + 30, 1) && is(TK_EV_SELECT, l, 26, 0), "a press selects a cell");
    mouse(1 + 2 * TK_CELL_W + 40, 1 + 30, 0);
    tk_list_layout(&tk, l, TK_ROWS);
    check(tk_item_rect(&tk, l, ev.a).h == TK_ROW_H, "the layout changes, the selection in view");
    check(draw_inside(), "a list draws inside its own box");

    // Eleven letters are drawn whole; a longer name is cut to fit. Either
    // way the cell's edges stay clear, so two long names side by side
    // stay apart, and the selection stays inside.
    fresh();
    static const tk_item_t wide[] = { { "WWWWWWWWWWWW", 0, TK_ICON_DOC }, { "WWWWWWWWWWW", 0, TK_ICON_DOC } };
    l = tk_list(&tk, 0, 0, 3 * TK_CELL_W + 2, 2 * TK_CELL_H + 2, TK_ICONS);
    tk_list_set(&tk, l, wide, 2);
    tk_list_select(&tk, l, -1);
    draw();
    r = tk_item_rect(&tk, l, 0);
    tk_rect_t r1 = tk_item_rect(&tk, l, 1);
    int ly = r.y + 44;
    uint32_t bg = at(r.x + 1, r.y + TK_CELL_H - 2);
    check(at(r1.x + 4, ly + 5) == TK_C_TEXT && at(r1.x + TK_CELL_W - 5, ly + 5) == TK_C_TEXT, "an 11-letter name, whole");
    check(at(r.x + 1, ly + 5) == bg && at(r.x + TK_CELL_W - 2, ly + 5) == bg && at(r1.x + 1, ly + 5) == bg,
          "a 12-letter one cut, with clear space before the next");
    tk_list_select(&tk, l, 0);
    draw();
    check(at(r.x + TK_CELL_W - 2, ly - 1) != bg && at(r1.x + 1, ly - 1) == bg, "and its selection stays in its cell");
}

static const char* const file_items[] = { "WW", "-", "Quit" };
static const char* const view_items[] = { "Icons", "Rows" };
static const tk_menu_t menus[] = { { "File", file_items, 3 }, { "View", view_items, 2 } };

static void test_menus(void) {
    fresh();
    tk_menubar(&tk, menus, 2);
    int b = tk_button(&tk, 10, 40, 80, "b");
    draw();
    check(at(W - 1, 0) == TK_C_BAR && at(W - 1, TK_BAR_H - 1) != TK_C_BAR, "the menu bar, and its bottom line");
    tk_rect_t t0 = tk_menu_title_rect(&tk, 0), t1 = tk_menu_title_rect(&tk, 1);
    check(t0.x == 4 && t1.x == t0.x + t0.w && t1.w == 4 * 8 + 24, "the titles, side by side");

    check(!click(t0.x + 5, 5) && tk.menu == 0, "a click on a title drops its menu down");
    draw();
    tk_rect_t i0 = tk_menu_item_rect(&tk, 0, 0), i2 = tk_menu_item_rect(&tk, 0, 2);
    check(at(t0.x + 2, 2) == TK_C_FOCUS, "its title lit");
    check(at(i0.x + 2, i0.y + 2) == TK_C_WHITE && at(i0.x + 12, i0.y + 3) == TK_C_TEXT, "the menu over the window, its items");
    check(at(20, 50) == TK_C_WHITE, "over the button");
    check(!click(i0.x + 5, i0.y + TK_ITEM_H + 5) && tk.menu == 0, "the line between items is not one");
    check(click(i2.x + 5, i2.y + 5) && is(TK_EV_MENU, -1, 0, 2) && tk.menu == -1, "a click on an item chooses it");
    draw();
    check(at(i0.x + 2, i0.y + 20) == TK_C_FACE || at(i0.x + 2, i0.y + 20) == TK_C_BG, "and the menu is gone");

    click(t0.x + 5, 5);
    click(t0.x + 5, 5);
    check(tk.menu == -1, "a second click on its title puts it away");
    click(t0.x + 5, 5);
    mouse(t1.x + 5, 5, 0);
    check(tk.menu == 1, "moving along the bar moves to that menu");
    click(300, 250);
    check(tk.menu == -1 && tk.focus == b, "a click elsewhere puts it away, and nothing else");

    // Press on a title, drag down, let go on an item.
    mouse(t1.x + 5, 5, 1);
    tk_rect_t v1 = tk_menu_item_rect(&tk, 1, 1);
    mouse(v1.x + 5, v1.y + 5, 1);
    check(tk.hover == 1, "dragging down lights the item");
    check(mouse(v1.x + 5, v1.y + 5, 0) && is(TK_EV_MENU, -1, 1, 1), "letting go on it chooses it");
    mouse(t1.x + 5, 5, 1);
    check(!mouse(t1.x + 5, 5, 0) && tk.menu == 1, "letting go on the title leaves it down");
    click(300, 250);

    // The keys.
    check(!key(KEY_NAV(NAV_F1 + 9, 0)) && tk.menu == 0 && tk.hover == 0, "F10: the first menu, its first item");
    key(KEY_DOWN);
    check(tk.hover == 2, "Down skips the line");
    key(KEY_DOWN);
    check(tk.hover == 0, "and goes round");
    key(KEY_UP);
    check(tk.hover == 2, "Up too");
    key(KEY_RIGHT);
    check(tk.menu == 1 && tk.hover == -1, "Right: the next menu");
    key(KEY_RIGHT);
    check(tk.menu == 0, "and round");
    key(0x1B);
    check(tk.menu == -1, "Esc puts it away");
    key(KEY_NAV(NAV_F1 + 9, 0));
    check(key('\n') && is(TK_EV_MENU, -1, 0, 0), "Enter chooses");
    check(key('\n') && is(TK_EV_CLICK, b, 0, 0), "and then the keys are the button's again");
    key(KEY_NAV(NAV_F1 + 9, 0));
    check(draw_inside(), "a menu draws inside what it says");
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_FOCUS;
    tk_handle(&tk, &m, &ev);
    check(tk.menu == -1, "the window losing the keys puts the menu away");
}

static void test_dialog(void) {
    fresh();
    int b = tk_button(&tk, 10, 10, 80, "b");
    int f = tk_field(&tk, 10, 40, 80, "");
    (void)f;
    static const char* const yes_no[] = { "Yes", "No" };
    tk_dialog(&tk, 42, "Sure?", yes_no, 2, 0);
    check(draw_inside(), "a dialog draws inside what it says");
    tk_rect_t y0 = tk_dialog_button_rect(&tk, 0), n0 = tk_dialog_button_rect(&tk, 1);
    check(y0.w == 80 && n0.x == y0.x + 88 && y0.y == n0.y, "its buttons, in a row");
    tk_draw(&tk);
    check(at(y0.x + 1, y0.y + 1) == TK_C_FOCUS && at(n0.x + 1, n0.y + 1) == TK_C_FACE, "the first is the default");
    check(at(W / 2, H / 2 - 30) == 0xF4F5F7, "in a box in the middle of the window");
    check(!click(20, 20), "nothing else gets the mouse");
    check(!key(' ') && !key('x'), "or the keys");
    check(key('\t') == 0 && tk.dlg_sel == 1, "Tab moves the default");
    check(key('\n') && is(TK_EV_ANSWER, 42, 1, 0) && !tk.dlg, "Enter answers with it");
    check(key('\n') && is(TK_EV_CLICK, b, 0, 0), "and then the window has its keys again");

    tk_dialog(&tk, 7, "Sure?", yes_no, 2, 0);
    check(key(0x1B) && is(TK_EV_ANSWER, 7, 1, 0), "Esc: the last button");
    tk_dialog(&tk, 7, "Sure?", yes_no, 2, 0);
    mouse(n0.x + 5, n0.y + 5, 1);
    check(!mouse(300, 5, 0) && tk.dlg, "pressed on a button and let go off it: no answer");
    check(key('\n') && is(TK_EV_ANSWER, 7, 1, 0), "but it is the default now");
    tk_dialog(&tk, 7, "Sure?", yes_no, 2, 0);
    mouse(y0.x + 5, y0.y + 5, 1);
    check(mouse(y0.x + 6, y0.y + 6, 0) && is(TK_EV_ANSWER, 7, 0, 0), "on it: that button");

    static const char* const ok[] = { "OK", "Cancel" };
    tk_dialog(&tk, 3, "Name:", ok, 2, "ab");
    type("cW");
    check(!strcmp(tk_dialog_text(&tk), "abcW"), "a dialog's field takes the typing");
    key(KEY_LEFT);
    key('\b');
    check(!strcmp(tk_dialog_text(&tk), "abW"), "and edits");
    check(key('\n') && is(TK_EV_ANSWER, 3, 0, 0) && !strcmp(tk_dialog_text(&tk), "abW"), "Enter: OK, with the text");
    check(!strcmp(tk_text(&tk, f), ""), "the window's own field untouched");
    check(draw_inside(), "and it goes, drawing only where it was");
}

static void test_damage(void) {
    fresh();
    int a = tk_button(&tk, 10, 10, 80, "a");
    int b = tk_button(&tk, 200, 200, 80, "b");
    draw();
    tk_set_text(&tk, b, "c");
    tk_rect_t r = tk_draw(&tk);
    check(r.x == 200 && r.y == 200 && r.w == 80 && r.h == TK_LINE_H, "a change draws just that widget");
    key('\t');
    r = tk_draw(&tk);
    check(r.x == 10 && r.y == 10 && r.x + r.w == 280 && r.y + r.h == 200 + TK_LINE_H, "the focus moving: both");
    (void)a;
    int l = tk_label(&tk, 10, 100, 200, "WWW\nWWW");
    draw();
    tk_set_text(&tk, l, "W");
    r = tk_draw(&tk);
    check(r.h == 32 && at(10, 116) == TK_C_BG, "a label getting shorter clears what it had");
    tk_dirty(&tk, -50, -50, 60, 60);
    r = tk_draw(&tk);
    check(r.x == 0 && r.y == 0 && r.w == 10 && r.h == 10, "damage is clipped to the window");
    check(draw_inside(), "and nothing drawn when nothing changed");
    tk_dirty(&tk, 220, 205, 5, 5);  // inside button b
    check(draw_inside(), "part of a widget: just that part");
}

static void test_cut(void) {
    fresh();
    tk_label(&tk, 0, 0, 24, "aaaaa");
    draw();
    check(at(16, 0) == TK_C_TEXT && at(16, 5) == TK_C_BG && at(8, 0) == TK_C_BG, "a text cut short ends in a ~");
    tk_label(&tk, 0, 20, 24, "WWW");
    draw();
    check(at(16, 25) == TK_C_TEXT && at(24, 25) == TK_C_BG, "one that just fits is not cut");
    for (int i = 0; i < TK_MAX; i++) tk_label(&tk, 0, 0, 1, "");
    check(tk_label(&tk, 0, 0, 1, "") == -1, "a full table makes no more");
}

int main(void) {
    memset(font + 'W' * 16, 0xFF, 16);
    font['~' * 16] = 0xFF;
    test_buttons();
    test_focus();
    test_check();
    test_field();
    test_rows();
    test_typeahead();
    test_icons();
    test_menus();
    test_dialog();
    test_damage();
    test_cut();
    printf("test_tk: %d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}

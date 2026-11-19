// Kenoma, the launcher: a window on pleroma's desktop with an icon for
// each window program, each document in the root directory and each
// text program there (a file with no dot that is an ELF program or a
// script; data with no dot is left out). Opening one (a double click, or Enter) starts it:
// a window program as itself, a document in Octavo and a text program
// in a terminal window (desk.h). The View menu shows them as icons or
// in rows; the list is read again whenever the window gets the keys.
//
// With no window server running, kenoma starts pleroma, which starts
// kenoma: "kenoma" at the shell is the way onto the desktop. Leaving
// it (pleroma's menu, or Ctrl+Alt+Backspace) comes back to the shell.
// The desktop wants a screen of at least 640x480.
#include "ulib.h"
#include "win.h"
#include "tk.h"
#include "desk.h"

#define MAX_ITEMS 160
#define MIN_W 640
#define MIN_H 480
#define STATUS_H 22

enum { K_APP, K_DOC, K_PROG };

typedef struct {
    char file[NAME_MAX + 1];  // on the disk
    char shown[NAME_MAX + 1];
    int kind;
} entry_t;

static entry_t entries[MAX_ITEMS];
static tk_item_t items[MAX_ITEMS];
static int count;

static win_t win;
static tk_t tk;
static int list, status;

static const char* const file_menu[] = { "Open", "About", "-", "Close" };
static const char* const view_menu[] = { "Icons", "Rows", "-", "Refresh" };
static const tk_menu_t menus[] = { { "File", file_menu, 4 }, { "View", view_menu, 4 } };
enum { M_OPEN, M_ABOUT, M_CLOSE = 3 };
enum { M_ICONS, M_ROWS, M_REFRESH = 3 };

static void lower(char* d, const char* s, size_t size) {
    size_t i = 0;
    for (; s[i] && i < size - 1; i++) d[i] = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
    d[i] = 0;
}

static void add(const char* file, const char* shown, int kind) {
    if (count >= MAX_ITEMS) return;
    entry_t* e = &entries[count++];
    memset(e, 0, sizeof *e);  // compared whole by refresh
    strlcpy(e->file, file, sizeof e->file);
    if (shown) strlcpy(e->shown, shown, sizeof e->shown);
    else lower(e->shown, file, sizeof e->shown);
    e->kind = kind;
}

// The window programs on the disk, then the documents and the text
// programs in the root, each in order by name.
static void scan(void) {
    count = 0;
    dirent_t d;
    for (int i = 0; i < desk_napps; i++)
        if (sys_stat(desk_apps[i].prog, &d) == 0) add(desk_apps[i].prog, desk_apps[i].name, K_APP);
    uint32_t n = 0;
    int err;
    dirent_t* all = read_dir("/", &n, &err);
    if (!all) return;
    sort_dirents(all, n);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++) {
            const char* name = all[i].name;
            if (all[i].flags & DIRENT_DIR) continue;
            if (pass == 0 && desk_document(name)) add(name, 0, K_DOC);
            if (pass == 1 && !strchr(name, '.') && !desk_app(name) && !desk_hidden(name) && desk_runnable(name))
                add(name, 0, K_PROG);
        }
    free(all);
}

static void fill_items(void) {
    static const int icons[] = { TK_ICON_APP, TK_ICON_DOC, TK_ICON_PROGRAM };
    for (int i = 0; i < count; i++) {
        items[i].text = entries[i].shown;
        items[i].note = 0;
        items[i].icon = icons[entries[i].kind];
    }
}

static void say(const char* s) { tk_set_text(&tk, status, s); }

static void describe(int i) {
    if (i < 0 || i >= count) {
        say("");
        return;
    }
    static const char* const what[] = { "opens a window", "opens in Octavo", "runs in a terminal window" };
    char buf[TK_TEXT_MAX];
    snprintf(buf, sizeof buf, "%s: %s", entries[i].shown, what[entries[i].kind]);
    say(buf);
}

// Reads the root again; keeps the selection on the same name.
static void refresh(void) {
    static entry_t before[MAX_ITEMS];
    int nbefore = count, sel = tk.wd[list].sel;
    char keep[NAME_MAX + 1] = "";
    if (sel >= 0 && sel < count) strlcpy(keep, entries[sel].file, sizeof keep);
    memcpy(before, entries, sizeof(entry_t) * (size_t)count);
    scan();
    if (count == nbefore && memcmp(before, entries, sizeof(entry_t) * (size_t)count) == 0) return;
    fill_items();
    tk_list_set(&tk, list, items, count);
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].file, keep) == 0) tk_list_select(&tk, list, i);
    describe(tk.wd[list].sel);
}

static void open_item(int i) {
    if (i < 0 || i >= count) return;
    entry_t* e = &entries[i];
    int r = e->kind == K_DOC ? desk_open_document(e->file) : desk_start(e->file);
    char buf[TK_TEXT_MAX];
    if (r < 0) snprintf(buf, sizeof buf, "%s: cannot start: %s", e->shown, strerror(r));
    else snprintf(buf, sizeof buf, "%s: started", e->shown);
    say(buf);
}

static void about(void) {
    char v[80], text[TK_DLG_MAX];
    if (sys_kinfo(KINFO_VERSION, v, sizeof v) < 0) strlcpy(v, "RingTail", sizeof v);
    char* nl = strchr(v, '\n');
    if (nl) *nl = 0;
    snprintf(text, sizeof text, "%s\n\nKenoma starts programs and opens\ndocuments. Double-click one, or\nselect it and press Enter.", v);
    static const char* const ok[] = { "OK" };
    tk_dialog(&tk, 0, text, ok, 1, 0);
}

// No window server: start one, which starts kenoma, and wait for it.
static int start_desktop(void) {
    const char* argv[] = { "PLEROMA", "KENOMA", 0 };
    int id = sys_spawn_args("PLEROMA", argv);
    if (id < 0) {
        printf("kenoma: cannot start pleroma: %s\n", strerror(id));
        return 1;
    }
    return sys_join(id);
}

int main(void) {
    gfx_info_t gi;
    sys_gfx_info(&gi);
    if (gi.width < MIN_W || gi.height < MIN_H) {
        printf("kenoma: the desktop needs a screen of at least %dx%d; this one is %dx%d\n", MIN_W, MIN_H, gi.width,
               gi.height);
        return 1;
    }
    int ww = MIN(6 * TK_CELL_W + 2 + TK_SCROLL_W, gi.width - 80);
    int wh = MIN(400, gi.height - 28 - 24 - 2 - 60);  // the bar, a title bar and border, and room to see the desktop
    int r = win_open(&win, ww, wh, "Programs");
    if (r == -ECONNREFUSED) return start_desktop();
    if (r < 0) {
        printf("kenoma: no window: %s\n", strerror(r));
        return 1;
    }
    tk_init(&tk, win.pixels, win.w, win.h, win_font());
    tk_menubar(&tk, menus, 2);
    list = tk_list(&tk, 0, TK_BAR_H, ww, wh - TK_BAR_H - STATUS_H, TK_ICONS);
    status = tk_label(&tk, 8, wh - STATUS_H + 3, ww - 16, "");
    scan();
    fill_items();
    tk_list_set(&tk, list, items, count);
    describe(0);

    for (;;) {
        tk_rect_t d = tk_draw(&tk);
        if (d.w) win_damage(&win, d.x, d.y, d.w, d.h);
        ws_msg_t m;
        if (win_event(&win, &m, -1) < 0 || m.type == WS_CLOSE) break;
        if (m.type == WS_FOCUS && m.focus.on) refresh();
        tk.now = sys_ticks();
        tk_event_t e;
        if (!tk_handle(&tk, &m, &e)) continue;
        if (e.type == TK_EV_SELECT) describe(e.a);
        else if (e.type == TK_EV_OPEN) open_item(e.a);
        else if (e.type == TK_EV_MENU && e.a == 0) {
            if (e.b == M_OPEN) open_item(tk.wd[list].sel);
            else if (e.b == M_ABOUT) about();
            else if (e.b == M_CLOSE) break;
        } else if (e.type == TK_EV_MENU && e.a == 1) {
            if (e.b == M_REFRESH) refresh();
            else tk_list_layout(&tk, list, e.b == M_ICONS ? TK_ICONS : TK_ROWS);
        }
    }
    win_close(&win);
    return 0;
}

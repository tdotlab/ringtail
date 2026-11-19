// Files, the file browser: a window on pleroma's desktop showing one
// directory, folders first, with each file's size. Opening an item (a
// double click, or Enter) goes into a folder, starts a program in the
// root and opens anything else in Octavo (desk.h). The path at the top
// can be typed into; Up, or Backspace in the list, goes to the parent.
// The File menu makes, renames and deletes, each through a dialog;
// Delete and F2 in the list do the last two. The listing is read again
// whenever the window gets the keys, and after each change.
//
// wfiles [directory]
#include "ulib.h"
#include "win.h"
#include "tk.h"
#include "desk.h"
#include "tty.h"

#define MAX_ITEMS 256
#define STATUS_H 22
#define PAD 6

typedef struct {
    char name[NAME_MAX + 1];
    char note[16];
    unsigned size, flags, mdate, mtime;
} entry_t;

static entry_t entries[MAX_ITEMS];
static tk_item_t items[MAX_ITEMS];
static int count, more;  // more: entries left out for want of room

static char dir[PATH_MAX] = "/";
static win_t win;
static tk_t tk;
static int up, path, list, status;

static const char* const file_menu[] = { "Open", "New folder", "Rename", "Delete", "-", "Close" };
static const char* const view_menu[] = { "Icons", "Rows", "-", "Refresh" };
static const char* const go_menu[] = { "Up", "Root" };
static const tk_menu_t menus[] = { { "File", file_menu, 6 }, { "View", view_menu, 4 }, { "Go", go_menu, 2 } };
enum { M_OPEN, M_NEW, M_RENAME, M_DELETE, M_CLOSE = 5 };
enum { M_ICONS, M_ROWS, M_REFRESH = 3 };
enum { M_UP, M_ROOT };

enum { D_INFO, D_NEW, D_RENAME, D_DELETE };  // dialog tags

static void say(const char* s) { tk_set_text(&tk, status, s); }

static void sayf(const char* fmt, ...) {
    char buf[TK_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(buf);
}

static void oops(const char* what, const char* name, int err) {
    char text[TK_DLG_MAX];
    snprintf(text, sizeof text, "Cannot %s %s:\n%s.", what, name, strerror(err));
    static const char* const ok[] = { "OK" };
    tk_dialog(&tk, D_INFO, text, ok, 1, 0);
}

static void size_note(char* out, size_t n, unsigned size) {
    if (size < 1024) snprintf(out, n, "%u B", size);
    else if (size < 1024 * 1024) snprintf(out, n, "%u.%u KB", size / 1024, size % 1024 * 10 / 1024);
    else snprintf(out, n, "%u.%u MB", size >> 20, (size & 0xFFFFF) * 10 >> 20);
}

// A path made absolute against dir, with "." and ".." worked out and no
// doubled or trailing slash. 0, or -ENAMETOOLONG.
static int resolve(char* out, const char* p) {
    char tmp[PATH_MAX * 2];
    if (p[0] == '/') strlcpy(tmp, p, sizeof tmp);
    else snprintf(tmp, sizeof tmp, "%s/%s", dir, p);
    size_t n = 0;
    out[n++] = '/';
    for (char* s = tmp; *s;) {
        while (*s == '/') s++;
        char* e = s;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - s);
        if (len == 0 || (len == 1 && s[0] == '.')) {
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 1 && out[n - 1] != '/') n--;  // drop the last name
            if (n > 1) n--;                           // and its slash
        } else {
            if (n > 1) out[n++] = '/';
            if (n + len >= PATH_MAX) return -ENAMETOOLONG;
            memcpy(out + n, s, len);
            n += len;
        }
        s = e;
    }
    out[n] = 0;
    return 0;
}

static void full(char* out, const char* name) {
    if (join_path(out, PATH_MAX, dir, name) < 0) out[0] = 0;
}

// Whether a file in dir is a program to start: one in the root, with no
// dot, that is an ELF program or a script.
static int program(const char* name) {
    if (strcmp(dir, "/") != 0 || strchr(name, '.')) return 0;
    char p[PATH_MAX];
    return join_path(p, sizeof p, dir, name) == 0 && desk_runnable(p);
}

// The directory's entries, folders first, each kind in order by name.
static int scan(void) {
    uint32_t n = 0;
    int err;
    dirent_t* all = read_dir(dir, &n, &err);
    count = more = 0;
    if (!all) return err;
    sort_dirents(all, n);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++) {
            int is_dir = (all[i].flags & DIRENT_DIR) != 0;
            if (is_dir != (pass == 0)) continue;  // folders, then files
            if (count == MAX_ITEMS) {
                more++;
                continue;
            }
            entry_t* e = &entries[count++];
            memset(e, 0, sizeof *e);
            strlcpy(e->name, all[i].name, sizeof e->name);
            e->size = all[i].size, e->flags = all[i].flags;
            e->mdate = all[i].mdate, e->mtime = all[i].mtime;
            if (e->flags & DIRENT_DIR) strlcpy(e->note, "folder", sizeof e->note);
            else size_note(e->note, sizeof e->note, e->size);
        }
    free(all);
    for (int i = 0; i < count; i++) {
        items[i].text = entries[i].name;
        items[i].note = entries[i].note;
        items[i].icon = entries[i].flags & DIRENT_DIR ? TK_ICON_FOLDER
                        : desk_document(entries[i].name)  ? TK_ICON_DOC
                        : program(entries[i].name) ? (desk_app(entries[i].name) ? TK_ICON_APP : TK_ICON_PROGRAM)
                                                    : TK_ICON_NONE;
    }
    return 0;
}

static void summary(void) {
    int folders = 0;
    for (int i = 0; i < count; i++) folders += entries[i].flags & DIRENT_DIR ? 1 : 0;
    if (more) sayf("%d folders, %d files, and %d more not shown", folders, count - folders, more);
    else sayf("%d folder%s, %d file%s", folders, folders == 1 ? "" : "s", count - folders,
              count - folders == 1 ? "" : "s");
}

static void describe(int i) {
    if (i < 0 || i >= count) {
        summary();
        return;
    }
    entry_t* e = &entries[i];
    char when[32] = "";
    if (e->mdate)
        snprintf(when, sizeof when, ", written %04u-%02u-%02u %02u:%02u", (e->mdate >> 9) + 1980, e->mdate >> 5 & 15,
                 e->mdate & 31, e->mtime >> 11, e->mtime >> 5 & 63);
    if (e->flags & DIRENT_DIR) sayf("%s: a folder%s", e->name, when);
    else sayf("%s: %u bytes%s", e->name, e->size, when);
}

static void select_name(const char* name) {
    for (int i = 0; i < count; i++)
        if (strcasecmp(entries[i].name, name) == 0) {
            tk_list_select(&tk, list, i);
            describe(i);
            return;
        }
    describe(-1);
}

// Shows dir: the title, the path and the listing. keep: a name to
// select, or 0 for the first.
static void show(const char* keep) {
    char title[WS_TITLE_MAX];
    snprintf(title, sizeof title, "Files: %s", dir);
    win_title(&win, title);
    tk_set_text(&tk, path, dir);
    int err = scan();
    tk_list_set(&tk, list, items, count);
    if (err < 0) {
        sayf("cannot read %s: %s", dir, strerror(err));
        return;
    }
    if (keep) select_name(keep);
    else describe(count ? 0 : -1);
}

// Goes to p (absolute or against dir). 0, or why not, with dir as it was.
static int go(const char* p, const char* keep) {
    char to[PATH_MAX];
    int r = resolve(to, p);
    if (r < 0) return r;
    if (strcmp(to, "/") != 0) {
        dirent_t d;
        r = sys_stat(to, &d);
        if (r < 0) return r;
        if (!(d.flags & DIRENT_DIR)) return -ENOTDIR;
    }
    strlcpy(dir, to, sizeof dir);
    show(keep);
    return 0;
}

static void go_up(void) {
    if (strcmp(dir, "/") == 0) return;
    char name[NAME_MAX + 1];
    const char* slash = strrchr(dir, '/');
    strlcpy(name, slash + 1, sizeof name);
    go("..", name);
}

static int selected(void) {
    int i = tk.wd[list].sel;
    return i >= 0 && i < count ? i : -1;
}

static void open_item(int i) {
    if (i < 0) return;
    entry_t* e = &entries[i];
    if (e->flags & DIRENT_DIR) {
        int r = go(e->name, 0);
        if (r < 0) oops("open", e->name, r);
        return;
    }
    char p[PATH_MAX];
    full(p, e->name);
    int run = program(e->name);
    int r = run ? desk_start(e->name) : desk_open_document(p);
    if (r < 0) oops("open", e->name, r);
    else sayf("%s: %s", e->name, run ? "started" : "opened in Octavo");
}

static void refresh(void) {
    int i = selected();
    char keep[NAME_MAX + 1] = "";
    if (i >= 0) strlcpy(keep, entries[i].name, sizeof keep);
    if (scan() < 0) {  // gone from under us: back to the root
        go("/", 0);
        return;
    }
    tk_list_set(&tk, list, items, count);
    if (keep[0]) select_name(keep);
    else describe(-1);
}

static const char* const make_btns[] = { "Create", "Cancel" };
static const char* const rename_btns[] = { "Rename", "Cancel" };
static const char* const delete_btns[] = { "Delete", "Cancel" };
static char target[NAME_MAX + 1];  // what the open dialog is about

static void ask_new(void) { tk_dialog(&tk, D_NEW, "Name of the new folder:", make_btns, 2, "new"); }

static void ask_rename(void) {
    int i = selected();
    if (i < 0) return;
    strlcpy(target, entries[i].name, sizeof target);
    char text[TK_DLG_MAX];
    snprintf(text, sizeof text, "New name for %s:", target);
    tk_dialog(&tk, D_RENAME, text, rename_btns, 2, target);
}

static void ask_delete(void) {
    int i = selected();
    if (i < 0) return;
    strlcpy(target, entries[i].name, sizeof target);
    char text[TK_DLG_MAX];
    if (entries[i].flags & DIRENT_DIR)
        snprintf(text, sizeof text, "Delete the folder %s?\nOnly an empty folder can be deleted.", target);
    else snprintf(text, sizeof text, "Delete %s?\nIt cannot be brought back.", target);
    tk_dialog(&tk, D_DELETE, text, delete_btns, 2, 0);
}

static void answered(int tag, int button) {
    if (button != 0 || tag == D_INFO) return;
    char p[PATH_MAX], q[PATH_MAX];
    const char* name = tk_dialog_text(&tk);
    int r = 0;
    switch (tag) {
    case D_NEW:
        if (!name[0]) return;
        full(p, name);
        r = sys_mkdir(p);
        if (r < 0) {
            oops("make", name, r);
            return;
        }
        refresh();
        select_name(name);
        break;
    case D_RENAME:
        if (!name[0] || strcmp(name, target) == 0) return;
        full(p, target);
        full(q, name);
        r = sys_rename(p, q);
        if (r < 0) {
            oops("rename", target, r);
            return;
        }
        refresh();
        select_name(name);
        break;
    case D_DELETE: {
        int i = selected();
        full(p, target);
        dirent_t d;
        r = sys_stat(p, &d);
        if (r == 0) r = d.flags & DIRENT_DIR ? sys_rmdir(p) : sys_delete(p);
        if (r < 0) {
            oops("delete", target, r);
            return;
        }
        refresh();
        if (count) tk_list_select(&tk, list, MIN(i, count - 1));
        describe(selected());
        break;
    }
    }
}

int main(int argc, char** argv) {
    gfx_info_t gi;
    sys_gfx_info(&gi);
    int ww = MIN(560, (int)gi.width - 80);
    int wh = MIN(400, (int)gi.height - 28 - 24 - 2 - 40);
    if (ww < 240 || wh < 160) {
        printf("wfiles: the screen is too small\n");
        return 1;
    }
    int r = win_open(&win, ww, wh, "Files");
    if (r < 0) {
        printf("wfiles: no window: %s\n", strerror(r));
        return 1;
    }
    tk_init(&tk, win.pixels, win.w, win.h, win_font());
    tk_menubar(&tk, menus, 3);
    int top = TK_BAR_H + PAD;
    up = tk_button(&tk, PAD, top, 48, "Up");
    path = tk_field(&tk, PAD + 48 + PAD, top, ww - 3 * PAD - 48, dir);
    int ly = top + TK_LINE_H + PAD;
    list = tk_list(&tk, 0, ly, ww, wh - ly - STATUS_H, TK_ROWS);
    status = tk_label(&tk, 8, wh - STATUS_H + 3, ww - 16, "");
    tk_set_focus(&tk, list);
    if (argc < 2 || go(argv[1], 0) < 0) show(0);

    for (;;) {
        tk_rect_t d = tk_draw(&tk);
        if (d.w) win_damage(&win, d.x, d.y, d.w, d.h);
        ws_msg_t m;
        if (win_event(&win, &m, -1) < 0 || m.type == WS_CLOSE) break;
        if (m.type == WS_FOCUS && m.focus.on && !tk.dlg) refresh();
        tk.now = sys_ticks();
        tk_event_t e;
        if (!tk_handle(&tk, &m, &e)) continue;
        switch (e.type) {
        case TK_EV_SELECT: describe(e.a); break;
        case TK_EV_OPEN: open_item(e.a); break;
        case TK_EV_CLICK:
            if (e.id == up) go_up();
            break;
        case TK_EV_SUBMIT:
            if (e.id == path) {
                const char* to = tk_text(&tk, path);
                r = go(to, 0);
                if (r < 0) {
                    char shown[PATH_MAX];
                    strlcpy(shown, to, sizeof shown);
                    tk_set_text(&tk, path, dir);
                    oops("go to", shown, r);
                } else {
                    tk_set_focus(&tk, list);
                }
            }
            break;
        case TK_EV_ANSWER: answered(e.id, e.a); break;
        case TK_EV_MENU:
            if (e.a == 0) {
                if (e.b == M_OPEN) open_item(selected());
                else if (e.b == M_NEW) ask_new();
                else if (e.b == M_RENAME) ask_rename();
                else if (e.b == M_DELETE) ask_delete();
                else if (e.b == M_CLOSE) goto out;
            } else if (e.a == 1) {
                if (e.b == M_REFRESH) refresh();
                else tk_list_layout(&tk, list, e.b == M_ICONS ? TK_ICONS : TK_ROWS);
            } else if (e.a == 2) {
                if (e.b == M_UP) go_up();
                else go("/", 0);
            }
            break;
        case TK_EV_KEY:
            if (tk.focus != list) break;
            if (e.a == '\b') go_up();
            else if (e.a == KEY_NAV(NAV_DELETE, 0)) ask_delete();
            else if (e.a == KEY_NAV(NAV_F1 + 1, 0)) ask_rename();
            else if (e.a == KEY_NAV(NAV_F1 + 4, 0)) refresh();
            break;
        }
    }
out:
    win_close(&win);
    return 0;
}

// A small toolkit for windows on pleroma's desktop: labels, buttons,
// check boxes, one-line text fields, lists (rows, or a grid of icons)
// with a scroll bar, a menu bar with menus that drop down, and a modal
// dialog box. It draws into a window's pixels and turns the server's
// events into what a program wants to know: this button was clicked,
// this item opened, that menu item chosen.
//
// It makes no system calls, so the host tests run it as it is. The
// program owns the window (win.h) and the loop:
//
//     tk_t tk;
//     tk_init(&tk, win.pixels, win.w, win.h, win_font());
//     int ok = tk_button(&tk, 10, 10, 80, "OK");
//     for (;;) {
//         tk_rect_t r = tk_draw(&tk);
//         if (r.w) win_damage(&win, r.x, r.y, r.w, r.h);
//         ws_msg_t m;
//         if (win_event(&win, &m, -1) < 0 || m.type == WS_CLOSE) break;
//         tk.now = sys_ticks();
//         tk_event_t e;
//         if (tk_handle(&tk, &m, &e) && e.type == TK_EV_CLICK && e.id == ok) break;
//     }
//
// Widgets sit where the program puts them and never move by themselves;
// they are numbered from 0 in the order they were made. Tab and
// Shift+Tab move the keys between the ones that take them (buttons,
// check boxes, fields and lists), as does a click. Nothing depends on
// where the pointer is without a button held: the server does not say
// when the pointer leaves a window, so a hover could stick.
#ifndef RINGTAIL_TK_H
#define RINGTAIL_TK_H

#include <stdint.h>
#include "wsys.h"

#define TK_MAX        48   // widgets in one tk_t
#define TK_TEXT_MAX   128  // bytes of a widget's text, with its 0
#define TK_MENU_ITEMS 12   // items in one menu
#define TK_DLG_MAX    256  // bytes of a dialog's message, with its 0
#define TK_DLG_BTNS   3

// Sizes.
#define TK_LINE_H   24   // a button, a check box, a field
#define TK_ROW_H    20   // a list's row
#define TK_CELL_W   96   // a list's icon, with its name under it (at most 11 letters)
#define TK_CELL_H   68
#define TK_SCROLL_W 14   // a list's scroll bar
#define TK_BAR_H    22   // the menu bar
#define TK_ITEM_H   22   // an item of a menu that dropped down
#define TK_DCLICK   50   // ticks between the clicks of a double click
#define TK_TYPEAHEAD 100 // ticks a list waits for the next letter of a name typed to it

// Colours, and the ones the tests look for.
#define TK_C_BG      0xEEF0F3  // behind everything
#define TK_C_TEXT    0x1C2430
#define TK_C_DIM     0x5A6472  // a list item's note
#define TK_C_LINE    0x8A94A3  // borders
#define TK_C_FACE    0xE2E6EB  // a button, a scroll bar's track
#define TK_C_DOWN    0xC4CBD4  // a button held down
#define TK_C_FOCUS   0x2E5C9A  // the focus's border; the selection with the keys
#define TK_C_SEL     0xC8D6EA  // the selection without them
#define TK_C_WHITE   0xFFFFFF
#define TK_C_BAR     0xE4E7EC  // the menu bar
#define TK_C_THUMB   0x9AA2AE
#define TK_C_FOLDER  0xF2C14E  // the folder icon's body

enum { TK_LABEL, TK_BUTTON, TK_CHECK, TK_FIELD, TK_LIST };
enum { TK_ROWS, TK_ICONS };  // a list's layout
enum { TK_ICON_NONE, TK_ICON_FOLDER, TK_ICON_DOC, TK_ICON_PROGRAM, TK_ICON_APP };

// What tk_handle says happened. id is the widget's number.
enum {
    TK_EV_NONE,
    TK_EV_CLICK,   // a button: clicked, or Enter or Space with the keys
    TK_EV_TOGGLE,  // a check box: a is now on (1) or off (0)
    TK_EV_CHANGE,  // a field: its text changed
    TK_EV_SUBMIT,  // a field: Enter
    TK_EV_SELECT,  // a list: item a is selected now
    TK_EV_OPEN,    // a list: item a double-clicked, or Enter on it
    TK_EV_MENU,    // the menu bar: item b of menu a chosen (id -1)
    TK_EV_ANSWER,  // the dialog: button a pressed (Enter: the first;
                   // Esc: the last); id is the tag it was opened with
    TK_EV_KEY,     // a key nothing here wanted: a its code, b its mods
};

typedef struct { int type, id, a, b; } tk_event_t;
typedef struct { int x, y, w, h; } tk_rect_t;

// A list's item. The list keeps a pointer to the program's array, so
// the array and its strings must outlive it (or the next tk_list_set).
// note, if not 0, goes at the right of a row.
typedef struct {
    const char* text;
    const char* note;
    int icon;  // TK_ICON_*
} tk_item_t;

// A menu of the menu bar. An item "-" is a line between the others.
typedef struct {
    const char* title;
    const char* const* items;
    int n;
} tk_menu_t;

typedef struct {
    int type, hidden;
    int x, y, w, h;
    char text[TK_TEXT_MAX];  // a label's (lines split at \n), a button's, a check box's, a field's
    int on;                  // a check box
    int cur, scroll;         // a field: the cursor, and the first character shown
    const tk_item_t* items;  // a list
    int count, sel, top, layout;  // sel -1 for none; top: the first row shown
} tk_widget_t;

typedef struct {
    uint32_t* px;  // w*h, 0xRRGGBB, rows packed
    int w, h;
    const uint8_t* font;  // 256 glyphs, 8x16, a byte a row, high bit left
    uint32_t now;         // the caller's clock, in ticks, for double clicks and typed names

    tk_widget_t wd[TK_MAX];
    int n;
    int focus;   // the widget with the keys, or -1
    int active;  // the window has the keys (WS_FOCUS)

    // The mouse.
    int buttons;
    int press;            // the widget a held press began in, or -1
    int armed;            // a button pressed, with the pointer still on it
    int thumb, thumb_dy;  // a scroll bar's thumb being dragged: the list, or -1
    uint32_t click_at;    // the last press on a list item, for double clicks
    int click_list, click_item;

    // Letters typed to a list, to go to the name they begin.
    char typed[32];
    int typed_n, typed_list;
    uint32_t typed_at;

    // The menu bar.
    const tk_menu_t* menus;
    int nmenus;
    int menu, hover;  // the menu dropped down and its item lit, or -1

    // The dialog.
    int dlg, dlg_tag;  // open, and the tag its answer carries
    char dlg_text[TK_DLG_MAX];
    const char* dlg_btn[TK_DLG_BTNS];
    int dlg_n, dlg_sel, dlg_has_field, dlg_armed;
    tk_widget_t dlg_field;

    tk_rect_t dirty;  // what tk_draw will draw; w 0 for nothing
    tk_rect_t clip;   // while drawing
} tk_t;

// Starts with no widgets and the whole window to draw.
void tk_init(tk_t* tk, uint32_t* pixels, int w, int h, const uint8_t* font);

// Widgets, each its number or -1 when the table is full. Buttons,
// check boxes and fields are TK_LINE_H high; a label as high as its
// lines.
int tk_label(tk_t* tk, int x, int y, int w, const char* text);
int tk_button(tk_t* tk, int x, int y, int w, const char* text);
int tk_check(tk_t* tk, int x, int y, int w, const char* text, int on);
int tk_field(tk_t* tk, int x, int y, int w, const char* text);
int tk_list(tk_t* tk, int x, int y, int w, int h, int layout);
// The menu bar, across the top of the window; menus must outlive it.
void tk_menubar(tk_t* tk, const tk_menu_t* menus, int n);

// A label's, button's, check box's or field's text (a field's cursor
// goes to its end). Says nothing (no TK_EV_CHANGE).
void tk_set_text(tk_t* tk, int id, const char* text);
const char* tk_text(const tk_t* tk, int id);
void tk_set_on(tk_t* tk, int id, int on);
void tk_hide(tk_t* tk, int id, int hidden);
void tk_set_focus(tk_t* tk, int id);
// A list's items: the first selected (none when there are none), shown
// from the top.
void tk_list_set(tk_t* tk, int id, const tk_item_t* items, int count);
void tk_list_layout(tk_t* tk, int id, int layout);
// Selects item i (-1 for none) and scrolls it into view.
void tk_list_select(tk_t* tk, int id, int i);

// A modal dialog in the middle of the window: the message (lines split
// at \n), a field if field is not 0 (starting with that text), and n
// buttons (1 to TK_DLG_BTNS, the strings must outlive it). Until it is
// answered nothing else gets the mouse or the keys. TK_EV_ANSWER
// carries tag; then tk_dialog_text is what was in its field.
void tk_dialog(tk_t* tk, int tag, const char* text, const char* const* buttons, int n, const char* field);
const char* tk_dialog_text(const tk_t* tk);

// Feeds a server event (WS_MOUSE, WS_KEY, WS_FOCUS; others are
// ignored). 1 with what it meant in *ev, else 0.
int tk_handle(tk_t* tk, const ws_msg_t* m, tk_event_t* ev);

// Part of the window to draw again.
void tk_dirty(tk_t* tk, int x, int y, int w, int h);
// Draws what changed since the last time into the pixels and returns
// the rectangle it drew (for win_damage); w 0 when nothing had.
tk_rect_t tk_draw(tk_t* tk);

// Where things are, for the tests and for programs placing things.
tk_rect_t tk_item_rect(const tk_t* tk, int id, int i);  // w 0 when it is scrolled out of view
tk_rect_t tk_menu_title_rect(const tk_t* tk, int menu);
tk_rect_t tk_menu_item_rect(const tk_t* tk, int menu, int item);
tk_rect_t tk_dialog_button_rect(const tk_t* tk, int i);
// Draws an icon (TK_ICON_*), size 16 or 32, anywhere in the pixels.
void tk_icon(tk_t* tk, int x, int y, int size, int icon);

#endif

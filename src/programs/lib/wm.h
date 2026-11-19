// The window manager's core, for pleroma: the windows, their order and
// focus, what is under a point, the parts of the screen that need
// drawing again, and drawing them. It makes no system calls, so the
// host tests run it as it is; pleroma does the talking to the kernel
// and to the clients.
//
// The screen is a top bar (a menu button, a button for each window, a
// clock) over a desktop, with the windows on it, bottom to top, and
// the menu over everything when it is open. Each window has a border,
// a title bar with a close button, and its content: pixels the client
// draws (0xRRGGBB, w*h of them, rows packed), which the compositor
// copies in. The pointer is not drawn here; pleroma puts it on top.
#ifndef RINGTAIL_WM_H
#define RINGTAIL_WM_H

#include <stdint.h>

#define WM_MAX        12   // windows
#define WM_TITLE_MAX  40   // bytes of a title, with its 0
#define WM_BAR_H      28   // the top bar
#define WM_TITLE_H    24   // a window's title bar
#define WM_BORDER     1
#define WM_SHADOW     5    // how far a window's shadow reaches right and down
#define WM_CLOSE_W    18   // the close button, square
#define WM_MENU_MAX   8
#define WM_MENU_W     160
#define WM_ITEM_H     22
#define WM_DAMAGE_MAX 8
#define WM_BTN_W      140  // a window's button on the bar
#define WM_BTN_X      104  // where the first one starts

// What is under a point.
enum {
    WM_DESKTOP,    // nothing
    WM_BAR,        // the bar, between its buttons
    WM_BAR_MENU,   // the bar's menu button
    WM_BAR_WIN,    // a window's button on the bar
    WM_TITLE,      // a window's title bar, not on its close button
    WM_CLOSE,      // a window's close button
    WM_CONTENT,    // inside a window
    WM_FRAME,      // a window's border
    WM_MENU_ITEM,  // an item of the open menu
    WM_MENU_GAP    // the open menu, between items (its border)
};

typedef struct { int x, y, w, h; } wm_rect_t;

typedef struct {
    int part;  // WM_*
    int win;   // the window, for WM_BAR_WIN to WM_FRAME; -1 otherwise
    int x, y;  // WM_CONTENT: the point in the window's content
    int item;  // WM_MENU_ITEM: which; -1 otherwise
} wm_hit_t;

typedef struct {
    int used;
    int x, y, w, h;          // the content, on the screen
    const uint32_t* pixels;  // w*h; 0 draws the content blank
    char title[WM_TITLE_MAX];
    int tag;                 // the caller's, untouched here
} wm_win_t;

typedef struct {
    int W, H;
    uint32_t* frame;      // W*H: what the screen should show
    const uint8_t* font;  // 256 glyphs, 8x16, a byte a row, high bit left
    wm_win_t win[WM_MAX];
    int order[WM_MAX], n; // the windows, bottom first
    int focus;            // the window keys go to, or -1
    int place;            // how many windows have been placed: the cascade
    char clock[24];
    int menu_open, menu_x, menu_y, menu_n, menu_hover;
    char menu_items[WM_MENU_MAX][24];
    wm_rect_t damage[WM_DAMAGE_MAX];
    int ndamage;
    uint32_t desk_top, desk_bottom;  // the desktop's colours, top to bottom
} wm_t;

// Starts with no windows and the whole screen to draw.
void wm_init(wm_t* wm, int W, int H, uint32_t* frame, const uint8_t* font);

// A new window with w x h of content, placed by the cascade, on top and
// focused. -1 when the table is full or the window cannot fit on the
// screen (or is empty).
int wm_add(wm_t* wm, int w, int h, const uint32_t* pixels, const char* title, int tag);
void wm_remove(wm_t* wm, int win);
// To the top, and focused.
void wm_raise(wm_t* wm, int win);
// Puts the window's frame (border included) at x, y, kept where its
// title bar can still be reached: below the bar and with at least
// WM_TITLE_H*2 pixels of it on the screen across.
void wm_move(wm_t* wm, int win, int x, int y);
void wm_set_title(wm_t* wm, int win, const char* title);
void wm_set_pixels(wm_t* wm, int win, const uint32_t* pixels);
void wm_set_clock(wm_t* wm, const char* text);
// The desktop: a gradient from top (under the bar) to bottom.
void wm_set_desktop(wm_t* wm, uint32_t top, uint32_t bottom);

// The frame of a window: border, title bar and content.
wm_rect_t wm_frame(const wm_t* wm, int win);
// The window at the top, or -1.
int wm_top(const wm_t* wm);

wm_hit_t wm_hit(const wm_t* wm, int x, int y);

// The menu: n items (at most WM_MENU_MAX) with its corner at x, y, or
// as near as fits. hover is the item drawn highlighted, or -1.
void wm_menu_open(wm_t* wm, int x, int y, const char* const* items, int n);
void wm_menu_close(wm_t* wm);
void wm_menu_hover(wm_t* wm, int item);
wm_rect_t wm_menu_rect(const wm_t* wm);

// Part of the screen to draw again: clipped to the screen, and merged
// with what is already waiting when they overlap or touch. Never more
// than WM_DAMAGE_MAX rectangles; past that they all become one.
void wm_damage(wm_t* wm, int x, int y, int w, int h);
// Part of a window's content, in its own coordinates.
void wm_damage_content(wm_t* wm, int win, int x, int y, int w, int h);
// Moves the waiting rectangles to out (room for WM_DAMAGE_MAX) and
// returns how many.
int wm_take_damage(wm_t* wm, wm_rect_t* out);

// Draws r (clipped to the screen) into the frame. Nothing outside it
// changes.
void wm_compose(wm_t* wm, wm_rect_t r);

// The desktop's colour on row y, and the colours tests look for.
#define WM_DESK_TOP    0x102A5C  // the desktop to start with: deep blue to teal
#define WM_DESK_BOTTOM 0x146E80
uint32_t wm_desktop(const wm_t* wm, int y);
#define WM_C_BAR       0xD8DCE2
#define WM_C_BORDER    0x1C2430
#define WM_C_TITLE     0x2E5C9A  // the focused window's title bar
#define WM_C_TITLE_OFF 0x8A94A3  // the others'
#define WM_C_CLOSE     0xC0392B
#define WM_C_BLANK     0xEEF0F3  // content with no pixels yet
#define WM_C_MENU      0xF4F5F7
#define WM_C_HOVER     0x6FA8FF
#define WM_C_TEXT      0x1C2430
#define WM_C_WHITE     0xFFFFFF

#endif

// A text terminal's screen: a grid of cells that bytes are written
// into, with the common ANSI escape sequences understood. The kernel
// console draws one on the screen; wterm draws one in a window. It
// makes no system calls and allocates nothing, so the host tests run
// it as it is.
//
// A cell is 16 bits, as VGA text has them: the character in the low
// byte, the attribute in the high one (foreground in its low nibble,
// background in its high, all 16 colours each, VGA order).
//
// Understood: \n (down a line, to its start), \r, \b (back one,
// across a wrap), \t (to the next multiple of 8), and ESC [ with
// A B C D G H f (cursor moves), J K (erasing), s u (save and restore
// the cursor), m (colours: 0 1 7 22 27 30-37 39 40-47 49 90-97
// 100-107) and ?25 l / h (cursor hidden, shown). Anything else is
// passed over. Like a VT100, writing the last column leaves the cursor
// on it, and the line wraps only when another character follows.
#ifndef RINGTAIL_VT_H
#define RINGTAIL_VT_H

#include <stdint.h>
#include <stddef.h>

#define VT_PARAMS 8

typedef struct {
    uint16_t* cells;      // cols * rows, rows packed
    int cols, rows;
    int row, col;         // the cursor
    int wrap_pending;     // wrote the last column; wrap on the next character
    int saved_row, saved_col;
    int cursor_hidden;
    uint8_t base_color;   // what vt_set_color chose; SGR 0 goes back to it
    uint8_t color;        // base plus any SGR changes: what new text gets
    int reverse, bold;
    int esc_state, esc_private, esc_n;
    int esc_params[VT_PARAMS];
    uint32_t scrolls;     // how many times the grid has scrolled, ever
} vt_t;

// Starts blank, cursor top left, in color.
void vt_init(vt_t* vt, uint16_t* cells, int cols, int rows, uint8_t color);
void vt_write(vt_t* vt, const char* s, size_t len);
void vt_putc(vt_t* vt, char c);
// Blank in the current colour, cursor top left.
void vt_clear(vt_t* vt);
// A clean slate after a full-screen program: the base colour, the
// cursor shown, any half-read escape sequence dropped, and clear.
void vt_reset(vt_t* vt);
// A new base colour, which text gets from now on.
void vt_set_color(vt_t* vt, uint8_t color);
// Erase the character before the cursor and move back onto it (line
// editing), across a wrap.
void vt_erase_back(vt_t* vt);
// A blank cell in the current colour.
uint16_t vt_blank(const vt_t* vt);

#endif

// A text terminal's screen (vt.h).
#include "vt.h"

enum { ST_PLAIN, ST_ESC, ST_CSI };

uint16_t vt_blank(const vt_t* vt) { return (uint16_t)(' ' | (vt->color << 8)); }

static void clear_cells(vt_t* vt, int from, int to) {
    uint16_t b = vt_blank(vt);
    for (int i = from; i < to; i++) vt->cells[i] = b;
}

static void scroll(vt_t* vt) {
    int n = (vt->rows - 1) * vt->cols;
    uint16_t* c = vt->cells;
    for (int i = 0; i < n; i++) c[i] = c[i + vt->cols];
    clear_cells(vt, n, n + vt->cols);
    vt->row = vt->rows - 1;
    vt->scrolls++;
}

static void newline(vt_t* vt) {
    vt->col = 0;
    if (++vt->row >= vt->rows) scroll(vt);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ANSI colour numbers run black, red, green, yellow, blue, magenta,
// cyan, white; VGA puts blue first.
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void sgr(vt_t* vt, int p) {
    uint8_t fg = vt->color & 0x0F, bg = (vt->color >> 4) & 0x0F, base = vt->base_color;
    if (vt->reverse) { uint8_t t = fg; fg = bg; bg = t; }
    if (p == 0) { fg = base & 0x0F; bg = (base >> 4) & 0x0F; vt->reverse = 0; vt->bold = 0; }
    else if (p == 1) { vt->bold = 1; fg |= 0x08; }
    else if (p == 22) { vt->bold = 0; fg &= 0x07; }
    else if (p == 7) vt->reverse = 1;
    else if (p == 27) vt->reverse = 0;
    else if (p >= 30 && p <= 37) fg = (uint8_t)((vt->bold ? 0x08 : 0) | ansi_to_vga[p - 30]);
    else if (p == 39) fg = base & 0x0F;
    else if (p >= 40 && p <= 47) bg = ansi_to_vga[p - 40];
    else if (p == 49) bg = (base >> 4) & 0x0F;
    else if (p >= 90 && p <= 97) fg = (uint8_t)(0x08 | ansi_to_vga[p - 90]);
    else if (p >= 100 && p <= 107) bg = (uint8_t)(0x08 | ansi_to_vga[p - 100]);
    if (vt->reverse) { uint8_t t = fg; fg = bg; bg = t; }
    vt->color = (uint8_t)((bg << 4) | fg);
}

static void csi(vt_t* vt, char final) {
    int n = vt->esc_n, *p = vt->esc_params;
    int p0 = n > 0 ? p[0] : 0;
    int count = p0 ? p0 : 1;
    int R = vt->rows, C = vt->cols, at = vt->row * C + vt->col;
    // Moving and erasing start the line afresh; a colour change only
    // affects what comes next, which still wraps.
    if (final != 'm') vt->wrap_pending = 0;
    if (vt->esc_private) {
        if (p0 == 25 && final == 'l') vt->cursor_hidden = 1;
        if (p0 == 25 && final == 'h') vt->cursor_hidden = 0;
        return;
    }
    switch (final) {
    case 'A': vt->row = clampi(vt->row - count, 0, R - 1); break;
    case 'B': vt->row = clampi(vt->row + count, 0, R - 1); break;
    case 'C': vt->col = clampi(vt->col + count, 0, C - 1); break;
    case 'D': vt->col = clampi(vt->col - count, 0, C - 1); break;
    case 'G': vt->col = clampi(count - 1, 0, C - 1); break;
    case 'H': case 'f':
        vt->row = clampi((p0 ? p0 : 1) - 1, 0, R - 1);
        vt->col = clampi((n > 1 && p[1] ? p[1] : 1) - 1, 0, C - 1);
        break;
    case 'J':
        if (p0 == 0) clear_cells(vt, at, C * R);
        else if (p0 == 1) clear_cells(vt, 0, at + 1);
        else if (p0 == 2) clear_cells(vt, 0, C * R);
        break;
    case 'K':
        if (p0 == 0) clear_cells(vt, at, vt->row * C + C);
        else if (p0 == 1) clear_cells(vt, vt->row * C, at + 1);
        else if (p0 == 2) clear_cells(vt, vt->row * C, vt->row * C + C);
        break;
    case 'm':
        if (n == 0) sgr(vt, 0);
        for (int i = 0; i < n; i++) sgr(vt, p[i]);
        break;
    case 's': vt->saved_row = vt->row; vt->saved_col = vt->col; break;
    case 'u': vt->row = vt->saved_row; vt->col = vt->saved_col; break;
    }
}

// 1 if c belongs to an escape sequence.
static int escape(vt_t* vt, char c) {
    if (vt->esc_state == ST_ESC) {
        if (c == '[') {
            vt->esc_state = ST_CSI;
            vt->esc_private = 0;
            vt->esc_n = 0;
            vt->esc_params[0] = 0;
        } else {
            vt->esc_state = ST_PLAIN;  // ESC c (reset) and others: ignored
        }
        return 1;
    }
    if (vt->esc_state == ST_CSI) {
        if (c == '?' && vt->esc_n == 0) {
            vt->esc_private = 1;
        } else if (c >= '0' && c <= '9') {
            if (vt->esc_n == 0) vt->esc_n = 1;
            int* p = &vt->esc_params[vt->esc_n - 1];
            if (*p < 1000) *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (vt->esc_n == 0) vt->esc_n = 1;
            if (vt->esc_n < VT_PARAMS) vt->esc_params[vt->esc_n++] = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt->esc_state = ST_PLAIN;
            csi(vt, c);
        } else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x3F) {
            vt->esc_state = ST_PLAIN;  // malformed: give up on it
        }
        return 1;
    }
    if (c == 0x1B) {
        vt->esc_state = ST_ESC;
        return 1;
    }
    return 0;
}

void vt_putc(vt_t* vt, char c) {
    if (escape(vt, c)) return;
    switch (c) {
    case '\n': vt->wrap_pending = 0; newline(vt); return;
    case '\r': vt->wrap_pending = 0; vt->col = 0; return;
    case '\b':
        // Back over the last character written, even across a line
        // wrap, so "\b \b" erases it wherever it is. After the last
        // column the cursor is still on that character.
        if (vt->wrap_pending) vt->wrap_pending = 0;
        else if (vt->col > 0) vt->col--;
        else if (vt->row > 0) { vt->row--; vt->col = vt->cols - 1; }
        return;
    case '\t':
        if (vt->wrap_pending) { vt->wrap_pending = 0; newline(vt); }
        vt->col = (vt->col + 8) & ~7;
        if (vt->col >= vt->cols) { vt->col = vt->cols - 1; vt->wrap_pending = 1; }
        return;
    }
    if ((uint8_t)c < 0x20 || c == 0x7F) return;
    if (vt->wrap_pending) { vt->wrap_pending = 0; newline(vt); }
    vt->cells[vt->row * vt->cols + vt->col] = (uint16_t)((uint8_t)c | (vt->color << 8));
    if (vt->col == vt->cols - 1) vt->wrap_pending = 1;
    else vt->col++;
}

void vt_write(vt_t* vt, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) vt_putc(vt, s[i]);
}

void vt_clear(vt_t* vt) {
    clear_cells(vt, 0, vt->cols * vt->rows);
    vt->row = vt->col = 0;
    vt->wrap_pending = 0;
}

void vt_reset(vt_t* vt) {
    vt->color = vt->base_color;
    vt->reverse = vt->bold = 0;
    vt->cursor_hidden = 0;
    vt->esc_state = ST_PLAIN;
    vt_clear(vt);
}

void vt_set_color(vt_t* vt, uint8_t color) {
    vt->base_color = vt->color = color;
    vt->reverse = vt->bold = 0;
}

void vt_init(vt_t* vt, uint16_t* cells, int cols, int rows, uint8_t color) {
    *vt = (vt_t){ 0 };
    vt->cells = cells;
    vt->cols = cols;
    vt->rows = rows;
    vt_set_color(vt, color);
    vt_clear(vt);
}

void vt_erase_back(vt_t* vt) {
    if (vt->wrap_pending) vt->wrap_pending = 0;  // the cursor is still on the last character
    else if (vt->col > 0) vt->col--;
    else if (vt->row > 0) { vt->row--; vt->col = vt->cols - 1; }
    vt->cells[vt->row * vt->cols + vt->col] = vt_blank(vt);
}

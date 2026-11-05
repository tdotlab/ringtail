// Octavo for RingTail: a small text editor for the terminal.
//
// This is a port of Octavo (github.com/tdotlab/octavo), a Win32 editor
// written in C++ to learn how an editor works. The design is the same:
// one text buffer with the caret and the selection anchor as offsets
// into it, rows laid out from the buffer with soft word wrap, undo by
// whole-document snapshots with typing grouped into one step, find and
// replace that match case by default, and a two-line footer with a
// shortcut hint above the caret readout.
//
// What changed is the surface. It draws an 80x25 terminal with ANSI
// escapes instead of GDI, keeping a copy of what is on screen so each
// keystroke sends only the cells that changed. A few shortcuts moved
// because a terminal cannot tell them apart: Ctrl+H is Backspace, so
// Replace is Ctrl+R, and Ctrl+Shift+S arrives as Ctrl+S, so Save as
// hylic is F12. Alt+F4 becomes Ctrl+Q. Files are bytes, shown one
// column each; there is no mouse.
//
//   run octavo [file]
#include "ulib.h"

#define COLS 80
#define ROWS 25
#define TEXT_ROWS 23
#define HINT_ROW 23
#define STATUS_ROW 24
#define WRAP_COLS (COLS - 1)  // leave a column for the caret after a full row
#define TAB_COLS 4
#define MAX_FILE (1024u * 1024u)  // what SYS_SAVE accepts
#define HISTORY_LIMIT 100
#define HISTORY_BYTES (4u * 1024u * 1024u)
#define TYPING_GAP_TICKS 200  // 2 seconds

// ---- Keys ----
// Printable bytes and control characters stand for themselves. The
// rest come from the escape sequences raw mode delivers.
enum { NAV_UP, NAV_DOWN, NAV_RIGHT, NAV_LEFT, NAV_HOME, NAV_END,
       NAV_INSERT, NAV_DELETE, NAV_PGUP, NAV_PGDN, NAV_F1 };
#define M_SHIFT 1
#define M_CTRL 2
#define K_NAV(k, m) (0x100 | ((k) << 2) | (m))
#define K_IS_NAV(k) ((k) >= 0x100 && (k) < 0x200)
#define K_KEY(k) (((k) >> 2) & 0x3F)
#define K_MODS(k) ((k) & 3)
#define K_ALT(c) (0x200 | (c))
#define K_ESC 0x1B
#define K_FN(n) (NAV_F1 + (n) - 1)
#define CTRL(c) ((c) & 0x1F)

// ---- The document ----
static char* text;
static uint32_t len, cap;
static uint32_t cur, anchor;  // caret and selection anchor; equal when nothing is selected
static int at_row_end;        // the caret sits at the end of a wrapped row, not the start of the next
static int goal_col = -1;     // column kept by Up and Down
static int wrap = 1;
static uint32_t top;          // first visible row
static int left;              // first visible column, without wrap
static char filename[PATH_MAX];  // "" until the document has a name
static int crlf;              // the file used CR LF; it is saved the same way
static uint32_t doc_id, saved_id, next_id;

static char* clip;
static uint32_t clip_len;

static char message[COLS + 1];
static int quit;

static char find_term[64], replace_term[64];
static int match_case = 1;

static int has_byte(const char* s, uint32_t n, char c) {
    for (uint32_t i = 0; i < n; i++) if (s[i] == c) return 1;
    return 0;
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

static int char_cols(char c, int col) { return c == '\t' ? TAB_COLS - col % TAB_COLS : 1; }

static void set_message(const char* fmt, const char* arg) {
    snprintf(message, sizeof message, fmt, arg);
}

static int reserve(uint32_t n) {
    if (n <= cap) return 1;
    uint32_t c = cap ? cap : 4096;
    while (c < n) c *= 2;
    char* t = realloc(text, c);
    if (!t) { set_message("Out of memory%s", ""); return 0; }
    text = t;
    cap = c;
    return 1;
}

static int insert(uint32_t at, const char* s, uint32_t n) {
    if (len + n > MAX_FILE) { set_message("Octavo holds at most 1 MB%s", ""); return 0; }
    if (!reserve(len + n)) return 0;
    memmove(text + at + n, text + at, len - at);
    memcpy(text + at, s, n);
    len += n;
    return 1;
}

static void erase(uint32_t from, uint32_t to) {
    memmove(text + from, text + to, len - to);
    len -= to - from;
}

static uint32_t sel_from(void) { return MIN(cur, anchor); }
static uint32_t sel_to(void) { return MAX(cur, anchor); }
static int unsaved(void) { return doc_id != saved_id; }

// ---- Rows ----
// The layout is rebuilt from the buffer before every draw. A row is
// [start, end) of one line; a wrapped row's end is the next row's start.
typedef struct { uint32_t start, end; } row_t;
static row_t* rows;
static uint32_t nrows, rows_cap;

static void push_row(uint32_t start, uint32_t end) {
    if (nrows == rows_cap) {
        uint32_t c = rows_cap ? rows_cap * 2 : 256;
        row_t* r = realloc(rows, c * sizeof *r);
        if (!r) return;  // keep what fits; the screen shows less
        rows = r;
        rows_cap = c;
    }
    rows[nrows].start = start;
    rows[nrows].end = end;
    nrows++;
}

// Break at the last blank that fits, letting blanks hang past the edge
// as Octavo does; a word longer than the row is cut where it overflows.
static void wrap_line(uint32_t start, uint32_t end) {
    uint32_t rs = start;
    for (;;) {
        uint32_t p = rs;
        int col = 0;
        while (p < end) {
            int w = char_cols(text[p], col);
            if (col + w > WRAP_COLS) break;
            col += w;
            p++;
        }
        if (p == end) { push_row(rs, end); return; }
        uint32_t cut = p > rs ? p : rs + 1;
        uint32_t brk = 0;
        if (is_blank(text[cut])) {
            brk = cut;
            while (brk < end && is_blank(text[brk])) brk++;
        } else {
            for (uint32_t q = cut; q > rs; q--)
                if (is_blank(text[q - 1])) { brk = q; break; }
        }
        if (brk <= rs) brk = cut;
        if (brk >= end) { push_row(rs, end); return; }
        push_row(rs, brk);
        rs = brk;
    }
}

static void layout(void) {
    nrows = 0;
    uint32_t ls = 0;
    for (;;) {
        uint32_t le = ls;
        while (le < len && text[le] != '\n') le++;
        if (wrap) wrap_line(ls, le); else push_row(ls, le);
        if (le >= len) break;
        ls = le + 1;
    }
    if (nrows == 0) push_row(0, 0);
}

// The row holding an offset. At a soft wrap the offset starts the next
// row, unless the caret was put at the end of the previous one.
static uint32_t row_of(uint32_t off, int row_end) {
    uint32_t lo = 0, hi = nrows - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (rows[mid].start <= off) lo = mid; else hi = mid - 1;
    }
    if (row_end && lo > 0 && rows[lo].start == off && rows[lo - 1].end == off) lo--;
    return lo;
}

static int col_of(uint32_t r, uint32_t off) {
    int col = 0;
    for (uint32_t p = rows[r].start; p < off && p < len; p++) col += char_cols(text[p], col);
    return col;
}

static int is_wrapped(uint32_t r) { return r + 1 < nrows && rows[r + 1].start == rows[r].end; }

// The offset in row r nearest to a column. Landing on the end of a
// wrapped row keeps the caret on that row.
static uint32_t offset_at(uint32_t r, int goal) {
    uint32_t p = rows[r].start;
    int col = 0;
    while (p < rows[r].end) {
        int w = char_cols(text[p], col);
        if (col + w > goal) {
            if (goal - col > col + w - goal) p++;
            break;
        }
        col += w;
        p++;
    }
    at_row_end = p == rows[r].end && is_wrapped(r);
    return p;
}

// ---- Undo ----
typedef struct {
    char* text;
    uint32_t len, cur, anchor, top, id;
} snap_t;

static snap_t undo_stack[HISTORY_LIMIT], redo_stack[HISTORY_LIMIT];
static int nundo, nredo;
static uint32_t history_bytes;
static int typing_open;
static uint32_t typing_cur, typing_len, typing_tick;

static void drop_oldest(snap_t* stack, int* n) {
    history_bytes -= stack[0].len;
    free(stack[0].text);
    memmove(stack, stack + 1, (size_t)(*n - 1) * sizeof *stack);
    (*n)--;
}

static void clear_stack(snap_t* stack, int* n) {
    while (*n) drop_oldest(stack, n);
}

// Snapshots are bounded by count and by bytes: old steps go first.
static int capture(snap_t* s) {
    while (nundo + nredo > 0 && history_bytes + len > HISTORY_BYTES) {
        if (nundo) drop_oldest(undo_stack, &nundo); else drop_oldest(redo_stack, &nredo);
    }
    s->text = malloc(len ? len : 1);
    while (!s->text && (nundo || nredo)) {
        if (nundo) drop_oldest(undo_stack, &nundo); else drop_oldest(redo_stack, &nredo);
        s->text = malloc(len ? len : 1);
    }
    if (!s->text) return 0;
    memcpy(s->text, text, len);
    s->len = len;
    s->cur = cur;
    s->anchor = anchor;
    s->top = top;
    s->id = doc_id;
    return 1;
}

static void push_snap(snap_t* stack, int* n, snap_t s) {
    if (*n == HISTORY_LIMIT) drop_oldest(stack, n);
    stack[(*n)++] = s;
    history_bytes += s.len;
}

static int same_text(const snap_t* s) { return s->len == len && memcmp(s->text, text, len) == 0; }

// Call after an edit with the state captured before it.
static void record(snap_t* before, int ok) {
    typing_open = 0;
    at_row_end = 0;
    goal_col = -1;
    if (!ok || same_text(before)) { free(before->text); return; }
    clear_stack(redo_stack, &nredo);
    push_snap(undo_stack, &nundo, *before);
    doc_id = ++next_id;
}

// A run of typed characters shares one undo step while the caret stays
// where the last one left it and the typing does not pause.
static void type_text(const char* s, uint32_t n) {
    uint32_t now = sys_ticks();
    int continuing = typing_open && nundo && cur == anchor && cur == typing_cur &&
                     len == typing_len && now - typing_tick <= TYPING_GAP_TICKS;
    snap_t before;
    if (!continuing && !capture(&before)) { set_message("Out of memory%s", ""); return; }
    if (cur != anchor) { erase(sel_from(), sel_to()); cur = anchor = sel_from(); }
    if (insert(cur, s, n)) cur = anchor = cur + n;
    if (continuing) doc_id = ++next_id;
    else record(&before, 1);
    typing_open = 1;
    typing_cur = cur;
    typing_len = len;
    typing_tick = now;
    at_row_end = 0;
    goal_col = -1;
}

static void undo_redo(int redo) {
    snap_t* from = redo ? redo_stack : undo_stack;
    int* nfrom = redo ? &nredo : &nundo;
    if (!*nfrom) { set_message(redo ? "Nothing to redo%s" : "Nothing to undo%s", ""); return; }
    // Take the step off first: making room for the snapshot of the
    // current text may drop old steps.
    snap_t s = from[--*nfrom];
    history_bytes -= s.len;
    snap_t now;
    if (!capture(&now)) {
        push_snap(from, nfrom, s);
        set_message("Out of memory%s", "");
        return;
    }
    push_snap(redo ? undo_stack : redo_stack, redo ? &nundo : &nredo, now);
    free(text);
    text = s.text;
    len = s.len;
    cap = s.len ? s.len : 1;
    cur = s.cur;
    anchor = s.anchor;
    top = s.top;
    doc_id = s.id;
    typing_open = 0;
    at_row_end = 0;
    goal_col = -1;
}

// ---- Editing commands ----
static void delete_range(uint32_t from, uint32_t to) {
    if (from == to) return;
    snap_t before;
    if (!capture(&before)) { set_message("Out of memory%s", ""); return; }
    erase(from, to);
    cur = anchor = from;
    record(&before, 1);
}

static int word_class(char c) {
    if (c == ' ' || c == '\t') return 0;
    if (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (unsigned char)c >= 0x80) return 1;
    return 2;
}

// Line breaks are their own stops, so word motion never jumps over one.
static uint32_t next_word(uint32_t p) {
    if (p >= len) return len;
    if (text[p] == '\n') return p + 1;
    int wc = word_class(text[p]);
    if (wc != 0)
        while (p < len && text[p] != '\n' && word_class(text[p]) == wc) p++;
    while (p < len && text[p] != '\n' && word_class(text[p]) == 0) p++;
    return p;
}

static uint32_t prev_word(uint32_t p) {
    if (p == 0) return 0;
    if (text[p - 1] == '\n') return p - 1;
    while (p > 0 && text[p - 1] != '\n' && word_class(text[p - 1]) == 0) p--;
    if (p == 0 || text[p - 1] == '\n') return p;
    int wc = word_class(text[p - 1]);
    while (p > 0 && text[p - 1] != '\n' && word_class(text[p - 1]) == wc) p--;
    return p;
}

static void copy_selection(void) {
    uint32_t n = sel_to() - sel_from();
    if (!n) return;
    char* c = malloc(n);
    if (!c) { set_message("Out of memory%s", ""); return; }
    memcpy(c, text + sel_from(), n);
    free(clip);
    clip = c;
    clip_len = n;
}

static void paste(void) {
    if (!clip_len) return;
    snap_t before;
    if (!capture(&before)) { set_message("Out of memory%s", ""); return; }
    if (cur != anchor) { erase(sel_from(), sel_to()); cur = anchor = sel_from(); }
    int ok = insert(cur, clip, clip_len);
    if (ok) cur = anchor = cur + clip_len;
    record(&before, ok);
}

static void move_to(uint32_t p, int extend) {
    cur = p;
    if (!extend) anchor = cur;
}

static void move_vertical(int down, int extend, int page) {
    uint32_t r = row_of(cur, at_row_end);
    if (goal_col < 0) goal_col = col_of(r, cur);
    int goal = goal_col;
    uint32_t steps = page ? TEXT_ROWS - 1 : 1;
    uint32_t target = r;
    if (down) target = MIN(r + steps, nrows - 1);
    else target = r >= steps ? r - steps : 0;
    if (target == r && !page) {
        // Up on the first row goes home, Down on the last goes to the end.
        move_to(down ? len : 0, extend);
        at_row_end = 0;
    } else {
        move_to(offset_at(target, goal), extend);
    }
    if (page) {
        if (down) top = MIN(top + steps, nrows > TEXT_ROWS ? nrows - TEXT_ROWS : 0);
        else top = top >= steps ? top - steps : 0;
    }
    goal_col = goal;
}

// Home and End go to the ends of the caret's row; without wrap that is the line.
static void move_row_edge(int end, int extend) {
    uint32_t r = row_of(cur, at_row_end);
    move_to(end ? rows[r].end : rows[r].start, extend);
    at_row_end = end && is_wrapped(r);
    goal_col = -1;
}

// ---- Screen ----
// back is the frame being built, front what the terminal shows.
enum { A_TEXT, A_SEL, A_HINT, A_MSG, A_BAR, A_PROMPT, A_COUNT };
static const char* const sgr[A_COUNT] = {
    "\x1b[0m", "\x1b[0;30;47m", "\x1b[0;37m", "\x1b[0;93m", "\x1b[0;97;44m", "\x1b[0;97m",
};
static uint16_t back[ROWS][COLS], front[ROWS][COLS];

static char out[16384];
static uint32_t out_len;

static void out_flush(void) {
    if (out_len) sys_print(out, out_len);
    out_len = 0;
}

static void out_str(const char* s) {
    uint32_t n = strlen(s);
    if (out_len + n > sizeof out) out_flush();
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void put(int r, int c, char ch, int attr) {
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS)
        back[r][c] = (uint16_t)((uint8_t)ch | (attr << 8));
}

static void put_str(int r, int c, const char* s, int attr) {
    for (; *s && c < COLS; s++, c++) put(r, c, *s, attr);
}

static void fill_row(int r, int attr) {
    for (int c = 0; c < COLS; c++) put(r, c, ' ', attr);
}

static void invalidate(void) {
    memset(front, 0xFF, sizeof front);
    out_str("\x1b[0m\x1b[2J");
}

static void present(int caret_r, int caret_c) {
    char seq[24];
    int at_r = -1, at_c = -1, attr = -1;
    out_str("\x1b[?25l");
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            uint16_t cell = back[r][c];
            if (cell == front[r][c]) continue;
            if (r != at_r || c != at_c) {
                snprintf(seq, sizeof seq, "\x1b[%d;%dH", r + 1, c + 1);
                out_str(seq);
            }
            if ((cell >> 8) != attr) { attr = cell >> 8; out_str(sgr[attr]); }
            char ch[2] = { (char)(cell & 0xFF), 0 };
            out_str(ch);
            front[r][c] = cell;
            at_r = r;
            at_c = c + 1;
        }
    }
    snprintf(seq, sizeof seq, "\x1b[0m\x1b[%d;%dH\x1b[?25h", caret_r + 1, caret_c + 1);
    out_str(seq);
    out_flush();
}

static const char* const hint =
    "^F Find  ^R Replace  F3 Next  ^S Save  F12 Save as  ^Z/^Y Undo/Redo  F1 More";

static void draw_text(int* caret_r, int* caret_c) {
    layout();
    uint32_t cr = row_of(cur, at_row_end);
    if (top >= nrows) top = nrows - 1;
    if (cr < top) top = cr;
    if (cr >= top + TEXT_ROWS) top = cr - TEXT_ROWS + 1;
    int cc = col_of(cr, cur);
    if (wrap) left = 0;
    else if (cc < left) left = cc > 8 ? cc - 8 : 0;
    else if (cc >= left + COLS) left = cc - COLS + 9;
    *caret_r = (int)(cr - top);
    *caret_c = MIN(cc - left, COLS - 1);

    uint32_t sf = sel_from(), st = sel_to();
    for (int i = 0; i < TEXT_ROWS; i++) {
        fill_row(i, A_TEXT);
        uint32_t r = top + (uint32_t)i;
        if (r >= nrows) continue;
        int col = 0;
        uint32_t p;
        for (p = rows[r].start; p < rows[r].end; p++) {
            char ch = text[p];
            int w = char_cols(ch, col);
            int a = (p >= sf && p < st) ? A_SEL : A_TEXT;
            char shown = ch == '\t' ? ' ' : ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F) ? '?' : ch;
            for (int k = 0; k < w; k++) put(i, col + k - left, shown, a);
            col += w;
        }
        // A selected line break shows as one selected cell; a soft wrap has none.
        if (p < len && text[p] == '\n' && p >= sf && p < st) put(i, col - left, ' ', A_SEL);
    }
}

static void draw_status(void) {
    fill_row(STATUS_ROW, A_BAR);
    char left_text[48];
    snprintf(left_text, sizeof left_text, " %sOctavo - %s%s", unsaved() ? "* " : "",
             filename[0] ? filename : "Untitled", wrap ? "" : "  (no wrap)");
    put_str(STATUS_ROW, 0, left_text, A_BAR);
    uint32_t line = 1, line_start = 0;
    for (uint32_t p = 0; p < cur; p++)
        if (text[p] == '\n') { line++; line_start = p + 1; }
    char right[48];
    int n = snprintf(right, sizeof right, "Ln %u, Col %u", line, cur - line_start + 1);
    if (cur != anchor) snprintf(right + n, sizeof right - n, "  (%u selected)", sel_to() - sel_from());
    int x = COLS - 1 - (int)strlen(right);
    put_str(STATUS_ROW, x, right, A_BAR);
}

static void render(void) {
    int cr, cc;
    draw_text(&cr, &cc);
    fill_row(HINT_ROW, A_HINT);
    if (message[0]) put_str(HINT_ROW, 1, message, A_MSG);
    else put_str(HINT_ROW, 1, hint, A_HINT);
    draw_status();
    present(cr, cc);
}

// ---- Input ----
static uint8_t in_buf[256];
static int in_n, in_pos, in_full;

static int in_more(void) { return in_pos < in_n || in_full; }

static int in_byte(void) {
    while (in_pos >= in_n) {
        int n = sys_read(STDIN_FILENO, in_buf, sizeof in_buf);
        if (n <= 0) return -1;
        in_n = n;
        in_pos = 0;
        in_full = n == (int)sizeof in_buf;
    }
    return in_buf[in_pos++];
}

static int csi_mods(int p) {
    if (p < 2) return 0;
    p--;
    return ((p & 1) ? M_SHIFT : 0) | ((p & 4) ? M_CTRL : 0);
}

static int final_key(int f) {
    switch (f) {
    case 'A': return NAV_UP;
    case 'B': return NAV_DOWN;
    case 'C': return NAV_RIGHT;
    case 'D': return NAV_LEFT;
    case 'H': return NAV_HOME;
    case 'F': return NAV_END;
    case 'P': case 'Q': case 'R': case 'S': return NAV_F1 + (f - 'P');
    }
    return -1;
}

static int tilde_key(int n) {
    switch (n) {
    case 1: case 7: return NAV_HOME;
    case 4: case 8: return NAV_END;
    case 2: return NAV_INSERT;
    case 3: return NAV_DELETE;
    case 5: return NAV_PGUP;
    case 6: return NAV_PGDN;
    }
    if (n >= 11 && n <= 15) return NAV_F1 + n - 11;
    if (n >= 17 && n <= 21) return NAV_F1 + 5 + n - 17;
    if (n == 23 || n == 24) return NAV_F1 + 10 + n - 23;
    return -1;
}

// One key: a byte, a decoded escape sequence, Alt+byte, or Esc alone.
// Returns 0 for a sequence it does not know, -1 at end of input.
static int read_key(void) {
    int c = in_byte();
    if (c != 0x1B) return c;
    if (!in_more()) return K_ESC;
    c = in_byte();
    if (c == '[' || c == 'O') {
        int params[4] = { 0 }, np = 0, f;
        if (c == 'O') {
            f = in_byte();
        } else {
            for (;;) {
                f = in_byte();
                if (f < 0) return -1;
                if (f >= '0' && f <= '9') {
                    if (!np) np = 1;
                    if (np <= 4 && params[np - 1] < 1000) params[np - 1] = params[np - 1] * 10 + (f - '0');
                } else if (f == ';') {
                    if (!np) np = 1;
                    if (np < 4) params[np++] = 0;
                } else {
                    break;
                }
            }
        }
        int k = f == '~' ? tilde_key(params[0]) : final_key(f);
        if (k < 0) return 0;
        return K_NAV(k, csi_mods(np >= 2 ? params[1] : 0));
    }
    if (c < 0) return -1;
    return K_ALT(c);
}

// ---- Prompts on the hint row ----
// Returns 1 on Enter, 0 on Esc. Tab toggles *toggle when given.
static int prompt(const char* label, char* buf, uint32_t size, int* toggle) {
    uint32_t n = strlen(buf);
    for (;;) {
        int cr, cc;
        draw_text(&cr, &cc);
        fill_row(HINT_ROW, A_PROMPT);
        char head[40];
        snprintf(head, sizeof head, " %s%s: ", label,
                 toggle ? (*toggle ? " [match case, Tab]" : " [any case, Tab]") : "");
        put_str(HINT_ROW, 0, head, A_PROMPT);
        int x = (int)strlen(head);
        int room = COLS - 1 - x;
        int skip = (int)n > room ? (int)n - room : 0;
        put_str(HINT_ROW, x, buf + skip, A_PROMPT);
        draw_status();
        present(HINT_ROW, x + (int)n - skip);
        int k = read_key();
        if (k < 0 || k == K_ESC || k >= 0x200 || k == CTRL('Q')) return 0;
        if (k == '\r' || k == '\n') return 1;
        if (k == '\t' && toggle) { *toggle = !*toggle; continue; }
        if (k == 0x7F || k == '\b') { if (n) buf[--n] = 0; continue; }
        if (k == 0x17) { while (n && buf[n - 1] == ' ') n--; while (n && buf[n - 1] != ' ') n--; buf[n] = 0; continue; }
        if (k >= 0x20 && k < 0x7F && n + 1 < size) { buf[n++] = (char)k; buf[n] = 0; }
    }
}

// A one-key question: returns the key pressed, lowered.
static int ask(const char* question) {
    int cr, cc;
    draw_text(&cr, &cc);
    fill_row(HINT_ROW, A_PROMPT);
    put_str(HINT_ROW, 1, question, A_PROMPT);
    draw_status();
    present(HINT_ROW, MIN((int)strlen(question) + 2, COLS - 1));
    int k = read_key();
    if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
    return k;
}

// ---- Files ----
static int has_dot(const char* s) { return strchr(s, '.') != 0; }

static int load(const char* name) {
    int fd = sys_open(name, O_RDONLY);
    if (fd < 0) return fd;
    char* buf = 0;
    uint32_t n = 0, size = 0;
    int r = 0;
    for (;;) {
        if (n == size) {
            if (size >= MAX_FILE + 1) { r = -EFBIG; break; }
            size = size ? size * 2 : 4096;
            if (size > MAX_FILE + 1) size = MAX_FILE + 1;
            char* b = realloc(buf, size);
            if (!b) { r = -ENOMEM; break; }
            buf = b;
        }
        int got = sys_read(fd, buf + n, size - n);
        if (got < 0) { r = got; break; }
        if (got == 0) break;
        n += (uint32_t)got;
    }
    sys_close(fd);
    if (r == 0 && n > MAX_FILE) r = -EFBIG;
    if (r == 0 && has_byte(buf, n, 0)) r = -EINVAL;
    if (r < 0) { free(buf); return r; }
    // CR LF becomes LF while editing and comes back on save.
    uint32_t w = 0;
    int saw_crlf = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n') { saw_crlf = 1; continue; }
        buf[w++] = buf[i];
    }
    free(text);
    text = buf;
    len = w;
    cap = size;
    crlf = saw_crlf;
    return 0;
}

static void reset_view(void) {
    cur = anchor = 0;
    top = 0;
    left = 0;
    at_row_end = 0;
    goal_col = -1;
    typing_open = 0;
    clear_stack(undo_stack, &nundo);
    clear_stack(redo_stack, &nredo);
    doc_id = saved_id = ++next_id;
}

static int write_file(const char* name) {
    const char* data = text;
    uint32_t n = len;
    char* conv = 0;
    if (crlf) {
        uint32_t lines = 0;
        for (uint32_t i = 0; i < len; i++) lines += text[i] == '\n';
        if (len + lines > MAX_FILE) return -EFBIG;
        conv = malloc(len + lines + 1);
        if (!conv) return -ENOMEM;
        n = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (text[i] == '\n') conv[n++] = '\r';
            conv[n++] = text[i];
        }
        data = conv;
    }
    int r = sys_save(name, data ? data : "", n);
    free(conv);
    return r;
}

static const char* save_error(int r) {
    if (r == -EINVAL) return "not a valid file name (none of \\ : * ? \" < > | in it)";
    return strerror(r);
}

// Save under a new name. default_ext goes on a name typed without one.
static int save_as(const char* label, const char* default_ext) {
    char name[PATH_MAX];
    strlcpy(name, filename, sizeof name);
    if (!prompt(label, name, sizeof name, 0) || !name[0]) return 0;
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base[0] && !has_dot(base) && strlen(name) + strlen(default_ext) < sizeof name)
        strlcpy(name + strlen(name), default_ext, sizeof name - strlen(name));
    int r = write_file(name);
    if (r < 0) {
        char m[COLS + 1];
        snprintf(m, sizeof m, "Could not save %s: %s. Your text is still here.", name, save_error(r));
        strlcpy(message, m, sizeof message);
        return 0;
    }
    strlcpy(filename, name, sizeof filename);
    saved_id = doc_id;
    set_message("Saved %s", filename);
    return 1;
}

static int save(void) {
    // New documents are lemurs.
    if (!filename[0]) return save_as("Save as", ".lemur");
    int r = write_file(filename);
    if (r < 0) {
        char m[COLS + 1];
        snprintf(m, sizeof m, "Could not save %s: %s. Your text is still here.", filename, save_error(r));
        strlcpy(message, m, sizeof message);
        return 0;
    }
    saved_id = doc_id;
    set_message("Saved %s", filename);
    return 1;
}

// Before replacing or closing the document: 1 to go ahead, 0 to keep it.
static int keep_or_discard(const char* question) {
    if (!unsaved()) return 1;
    for (;;) {
        int k = ask(question);
        if (k == 'y') return save();
        if (k == 'n') return 1;
        if (k == K_ESC || k < 0 || k >= 0x200) return 0;
    }
}

static void open_file(void) {
    if (!keep_or_discard("Save changes before opening another document? y/n, Esc cancels")) return;
    char name[PATH_MAX] = "";
    if (!prompt("Open", name, sizeof name, 0) || !name[0]) return;
    int r = load(name);
    if (r < 0) {
        char m[COLS + 1];
        snprintf(m, sizeof m, "Could not open %s: %s. The current document is kept.", name,
                 r == -EINVAL ? "it is not text" : r == -EFBIG ? "it is over 1 MB" : strerror(r));
        strlcpy(message, m, sizeof message);
        return;
    }
    strlcpy(filename, name, sizeof filename);
    reset_view();
}

static void new_file(void) {
    if (!keep_or_discard("Save changes before starting a new document? y/n, Esc cancels")) return;
    len = 0;
    filename[0] = 0;
    crlf = 0;
    reset_view();
}

// ---- Find and replace ----
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int match_at(uint32_t p, const char* s, uint32_t n) {
    if (p + n > len) return 0;
    for (uint32_t i = 0; i < n; i++) {
        char a = text[p + i], b = s[i];
        if (match_case ? a != b : lower(a) != lower(b)) return 0;
    }
    return 1;
}

// Search from `from`, wrapping around the document. Returns the match
// offset or -1.
static int search(uint32_t from, int forward) {
    uint32_t n = strlen(find_term);
    if (!n || n > len) return -1;
    uint32_t last = len - n;  // the last offset a match can start at
    if (forward) {
        for (uint32_t i = 0; i <= last; i++) {
            uint32_t p = (from + i) % (last + 1);
            if (match_at(p, find_term, n)) return (int)p;
        }
    } else {
        uint32_t start = from > 0 ? from - 1 : last;
        if (start > last) start = last;
        for (uint32_t i = 0; i <= last; i++) {
            uint32_t p = (start + (last + 1) - i) % (last + 1);
            if (match_at(p, find_term, n)) return (int)p;
        }
    }
    return -1;
}

static void not_found(void) {
    char m[COLS + 1];
    snprintf(m, sizeof m, "%s%s", match_case ? "No exact match found for: " : "No match found for: ",
             find_term);
    strlcpy(message, m, sizeof message);
}

static int find_next(int forward) {
    int p = search(forward ? sel_to() : sel_from(), forward);
    if (p < 0) { not_found(); return 0; }
    anchor = (uint32_t)p;
    cur = (uint32_t)p + strlen(find_term);
    at_row_end = 0;
    goal_col = -1;
    return 1;
}

static void find_dialog(void) {
    if (cur != anchor && sel_to() - sel_from() < sizeof find_term && !has_byte(text + sel_from(), sel_to() - sel_from(), '\n')) {
        memcpy(find_term, text + sel_from(), sel_to() - sel_from());
        find_term[sel_to() - sel_from()] = 0;
    }
    if (!prompt("Find", find_term, sizeof find_term, &match_case) || !find_term[0]) return;
    // Start at the selection so the first match can be the one under it.
    uint32_t keep = cur;
    cur = anchor = sel_from();
    if (!find_next(1)) cur = anchor = keep;
}

static int replace_all(void) {
    uint32_t n = strlen(find_term), rn = strlen(replace_term), count = 0;
    snap_t before;
    if (!capture(&before)) { set_message("Out of memory%s", ""); return 0; }
    for (uint32_t p = 0; p + n <= len;) {
        if (match_at(p, find_term, n)) {
            erase(p, p + n);
            if (!insert(p, replace_term, rn)) break;
            p += rn;
            count++;
        } else {
            p++;
        }
    }
    cur = anchor = MIN(cur, len);
    record(&before, 1);  // one undoable edit for the lot
    return (int)count;
}

static void replace_dialog(void) {
    if (!prompt("Replace", find_term, sizeof find_term, &match_case) || !find_term[0]) return;
    if (!prompt("Replace with", replace_term, sizeof replace_term, 0)) return;
    cur = anchor = sel_from();
    while (find_next(1)) {
        int k = ask("Replace this one? y yes, n skip, a all, Esc stop");
        if (k == 'y') {
            snap_t before;
            if (!capture(&before)) { set_message("Out of memory%s", ""); return; }
            uint32_t at = sel_from();
            erase(at, sel_to());
            int ok = insert(at, replace_term, strlen(replace_term));
            cur = anchor = at + (ok ? strlen(replace_term) : 0);
            record(&before, 1);
        } else if (k == 'n') {
            anchor = cur;
        } else if (k == 'a') {
            int count = replace_all();
            char m[COLS + 1];
            snprintf(m, sizeof m, "%d %s", count, count == 1 ? "occurrence matched." : "occurrences matched.");
            strlcpy(message, m, sizeof message);
            return;
        } else {
            return;
        }
    }
}

// ---- Keys to commands ----
static void handle(int k) {
    int extend = 0;
    if (K_IS_NAV(k)) {
        int key = K_KEY(k), mods = K_MODS(k);
        extend = mods & M_SHIFT;
        int ctrl = mods & M_CTRL;
        typing_open = 0;
        switch (key) {
        case NAV_LEFT:
        case NAV_RIGHT: {
            int right = key == NAV_RIGHT;
            if (ctrl) move_to(right ? next_word(cur) : prev_word(cur), extend);
            else if (cur != anchor && !extend) move_to(right ? sel_to() : sel_from(), 0);
            else if (right && cur < len) move_to(cur + 1, extend);
            else if (!right && cur > 0) move_to(cur - 1, extend);
            at_row_end = 0;
            goal_col = -1;
            return;
        }
        case NAV_UP: case NAV_DOWN: move_vertical(key == NAV_DOWN, extend, 0); return;
        case NAV_PGUP: case NAV_PGDN: move_vertical(key == NAV_PGDN, extend, 1); return;
        case NAV_HOME: case NAV_END:
            if (ctrl) { move_to(key == NAV_END ? len : 0, extend); at_row_end = 0; goal_col = -1; }
            else move_row_edge(key == NAV_END, extend);
            return;
        case NAV_DELETE:
            if (cur != anchor) delete_range(sel_from(), sel_to());
            else if (ctrl) delete_range(cur, next_word(cur));
            else if (cur < len) delete_range(cur, cur + 1);
            return;
        }
        if (key == K_FN(3)) {
            if (!find_term[0]) find_dialog(); else find_next(!extend);
        } else if (key == K_FN(12)) {
            save_as("Save as hylic", ".txt");
        } else if (key == K_FN(1)) {
            strlcpy(message, "^A all ^X/^C/^V cut/copy/paste ^O open ^N new Alt+Z wrap ^Q quit",
                    sizeof message);
        }
        return;
    }
    if (k == K_ALT('z') || k == K_ALT('Z')) {
        wrap = !wrap;
        at_row_end = 0;
        set_message(wrap ? "Word wrap on%s" : "Word wrap off%s", "");
        return;
    }
    if (k >= 0x200) return;
    switch (k) {
    case '\r': case '\n': {
        snap_t before;
        if (!capture(&before)) { set_message("Out of memory%s", ""); return; }
        if (cur != anchor) { erase(sel_from(), sel_to()); cur = anchor = sel_from(); }
        int ok = insert(cur, "\n", 1);
        if (ok) cur = anchor = cur + 1;
        record(&before, ok);
        return;
    }
    case 0x7F: case '\b':
        if (cur != anchor) delete_range(sel_from(), sel_to());
        else if (cur > 0) delete_range(cur - 1, cur);
        return;
    case 0x17:  // Ctrl+Backspace
        if (cur != anchor) delete_range(sel_from(), sel_to());
        else delete_range(prev_word(cur), cur);
        return;
    case CTRL('A'): anchor = 0; cur = len; at_row_end = 0; return;
    case CTRL('C'): copy_selection(); return;
    case CTRL('X'):
        copy_selection();
        delete_range(sel_from(), sel_to());
        return;
    case CTRL('V'): paste(); return;
    case CTRL('Z'): undo_redo(0); return;
    case CTRL('Y'): undo_redo(1); return;
    case CTRL('F'): find_dialog(); return;
    case CTRL('R'): replace_dialog(); return;
    case CTRL('S'): save(); return;
    case CTRL('O'): open_file(); return;
    case CTRL('N'): new_file(); return;
    case CTRL('L'): invalidate(); return;
    case CTRL('Q'):
        if (keep_or_discard("Save changes before closing? y/n, Esc cancels")) quit = 1;
        return;
    }
    if (k == '\t' || (k >= 0x20 && k < 0x7F)) {
        char c = (char)k;
        type_text(&c, 1);
    }
}

int main(int argc, char** argv) {
    if (sys_tty_mode(TTY_RAW | TTY_NOSIG) < 0) {
        printf("octavo: the terminal is not ours (run it in the foreground)\n");
        return 1;
    }
    reset_view();
    if (argc > 1) {
        strlcpy(filename, argv[1], sizeof filename);
        int r = load(argv[1]);
        if (r == -ENOENT) set_message("New file: %s", filename);
        else if (r < 0) {
            char m[COLS + 1];
            snprintf(m, sizeof m, "Could not open %s: %s", argv[1],
                     r == -EINVAL ? "it is not text" : r == -EFBIG ? "it is over 1 MB" : strerror(r));
            strlcpy(message, m, sizeof message);
            filename[0] = 0;
        }
    }
    invalidate();
    while (!quit) {
        render();
        int k = read_key();
        if (k < 0) break;
        message[0] = 0;
        handle(k);
    }
    sys_tty_mode(0);
    print("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}

// Host test for the terminal screen (src/lib/vt.c) that the kernel
// console and wterm share: text and wrapping, scrolling, the control
// characters, every escape sequence it knows, colours, and what it does
// with sequences it does not.
#include <stdio.h>
#include <string.h>
#include "vt.h"

static int fails, checks;

static void check(int ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

#define W 10
#define H 4
#define C0 0x07  // light grey on black
static uint16_t cells[W * H + 1];  // one past the end, to catch a write there
static vt_t vt;

static void start(void) {
    cells[W * H] = 0xBEEF;
    vt_init(&vt, cells, W, H, C0);
}

static void put(const char* s) { vt_write(&vt, s, strlen(s)); }

static char ch(int r, int c) { return (char)(cells[r * W + c] & 0xFF); }
static int attr(int r, int c) { return cells[r * W + c] >> 8; }

// Row r's characters, trailing blanks dropped.
static const char* line(int r) {
    static char buf[W + 1];
    for (int c = 0; c < W; c++) buf[c] = ch(r, c);
    int n = W;
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    return buf;
}

static int at(int r, int c) { return vt.row == r && vt.col == c; }

static int blank_row(int r) { return line(r)[0] == 0; }

int main(void) {
    start();
    check(blank_row(0) && blank_row(H - 1) && attr(0, 0) == C0 && at(0, 0), "starts blank, cursor home");

    put("hello");
    check(!strcmp(line(0), "hello") && at(0, 5) && attr(0, 4) == C0, "plain text");
    put("\r\nab\rX");
    check(!strcmp(line(1), "Xb") && at(1, 1), "\\r goes to the start of the line, \\n down");
    put("\nq");
    check(!strcmp(line(2), "q") && at(2, 1), "\\n also goes to the start");

    // The last column: the cursor stays on it until another character.
    start();
    put("0123456789");
    check(at(0, 9) && vt.wrap_pending && blank_row(1), "the last column does not wrap yet");
    put("a");
    check(!strcmp(line(1), "a") && at(1, 1), "the next character wraps");
    start();
    put("0123456789\n");
    check(at(1, 0) && blank_row(1), "a newline after the last column is just one line");
    start();
    put("0123456789\r");
    check(at(0, 0) && !vt.wrap_pending, "and \\r cancels the wrap");
    start();
    put("\x1b[4;1H0123456789");
    check(vt.scrolls == 0 && !strcmp(line(3), "0123456789") && at(3, 9),
          "the bottom right cell fills without scrolling");

    // Scrolling.
    start();
    put("1\n2\n3\n4");
    check(vt.scrolls == 0 && !strcmp(line(3), "4"), "four lines fit");
    put("\x1b[31m\n5");
    check(vt.scrolls == 1 && !strcmp(line(0), "2") && !strcmp(line(3), "5") && at(3, 1), "a fifth scrolls once");
    check(attr(3, 5) == 0x04, "the new line is blank in the current colour");
    check(cells[W * H] == 0xBEEF, "nothing past the grid is touched");

    // Backspace and tab.
    start();
    put("ab\b");
    check(at(0, 1) && !strcmp(line(0), "ab"), "\\b moves back without erasing");
    put("\b\b\b");
    check(at(0, 0), "and stops at the top left");
    start();
    put("0123456789x\b\b");
    check(at(0, 9), "\\b goes back across a wrap");
    start();
    put("0123456789\b");
    check(at(0, 9) && !vt.wrap_pending, "after the last column \\b stays on it");
    start();
    put("\tx");
    check(ch(0, 8) == 'x', "tab to column 8");
    put("\t");
    check(at(0, 9) && vt.wrap_pending, "a tab past the end stops at the last column");
    put("y");
    check(ch(1, 0) == 'y', "and the next character wraps");
    start();
    put("0123456789\tx");
    check(ch(1, 8) == 'x', "a tab after the last column wraps first");

    // Cursor moves.
    start();
    put("\x1b[3;5H");
    check(at(2, 4), "CSI H row;col");
    put("\x1b[H");
    check(at(0, 0), "CSI H alone is home");
    put("\x1b[2B\x1b[3C");
    check(at(2, 3), "CSI B and C");
    put("\x1b[A\x1b[2D");
    check(at(1, 1), "CSI A and D, one by default");
    put("\x1b[99B\x1b[99C");
    check(at(3, 9), "moves stop at the edges");
    put("\x1b[99A\x1b[99D");
    check(at(0, 0), "both ways");
    put("\x1b[7G");
    check(at(0, 6), "CSI G");
    put("\x1b[0;0f");
    check(at(0, 0), "CSI f, zeros meaning one");
    put("\x1b[2;99H");
    check(at(1, 9), "CSI H clamps");
    put("\x1b[1;0H");
    check(at(0, 0), "a zero column is the first");
    put("\x1b[;4H");
    check(at(0, 3), "an empty row is the first");
    put("\x1b[H\x1b[2?C");
    check(at(0, 2), "a ? after the parameters is passed over");
    put("\x1b[2;3H\x1b[s\x1b[4;9H\x1b[u");
    check(at(1, 2), "CSI s and u save and restore the cursor");
    put("0123456789");
    put("\x1b[C");
    check(!vt.wrap_pending, "a move cancels a pending wrap");

    // Erasing.
    start();
    put("AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDD");
    put("\x1b[2;5H\x1b[K");
    check(!strcmp(line(1), "BBBB") && !strcmp(line(0), "AAAAAAAAAA"), "CSI K erases to the end of the line");
    put("\x1b[3;5H\x1b[1K");
    check(!strcmp(line(2), "     CCCCC"), "CSI 1K to the cursor, the cursor's cell included");
    put("\x1b[2K");
    check(blank_row(2) && at(2, 4), "CSI 2K the whole line, the cursor staying");
    put("\x1b[4;3H\x1b[J");
    check(!strcmp(line(3), "DD") && !strcmp(line(0), "AAAAAAAAAA"), "CSI J to the end of the screen");
    put("\x1b[1;4H\x1b[1J");
    check(!strcmp(line(0), "    AAAAAA") && !strcmp(line(3), "DD"), "CSI 1J to the cursor");
    put("\x1b[2J");
    check(blank_row(0) && blank_row(3) && at(0, 3), "CSI 2J everything, the cursor staying");
    put("\x1b[44m\x1b[2J");
    check(attr(3, 9) == 0x17, "erasing uses the current colour");

    // Colours.
    start();
    put("\x1b[31ma");
    check(attr(0, 0) == 0x04, "SGR 31: red is VGA 4");
    put("\x1b[1mb");
    check(attr(0, 1) == 0x0C, "SGR 1 brightens it");
    put("\x1b[34mc");
    check(attr(0, 2) == 0x09, "and stays for the next colour");
    put("\x1b[22md");
    check(attr(0, 3) == 0x01, "SGR 22 takes it off");
    put("\x1b[42me");
    check(attr(0, 4) == 0x21, "SGR 42: a green background");
    put("\x1b[7mf");
    check(attr(0, 5) == 0x12, "SGR 7 swaps them");
    put("\x1b[33mg");
    check(attr(0, 6) == 0x62, "a colour while reversed sets the foreground as seen");
    put("\x1b[27mh");
    check(attr(0, 7) == 0x26, "SGR 27 swaps back");
    put("\x1b[39;49mi");
    check(attr(0, 8) == C0, "SGR 39 and 49: the base colours");
    put("\x1b[95;104mj");
    check(attr(0, 9) == 0x9D, "SGR 95 and 104: the bright ones");
    put("\x1b[107m");
    check(vt.color == 0xFD, "SGR 107: bright white behind");
    put("\x1b[1;7;32m\x1b[mk");
    check(ch(0, 9) == 'j', "a colour change does not cancel a pending wrap");
    check(attr(1, 0) == C0 && !vt.bold && !vt.reverse, "SGR alone resets everything");
    put("\x1b[1;32mx\x1b[0m");
    check(attr(1, 1) == 0x0A && vt.color == C0, "parameters in one sequence, then SGR 0");
    put("\x1b[38m\x1b[5m\x1b[50m");
    check(vt.color == C0, "unknown SGRs change nothing");
    vt_set_color(&vt, 0x1E);
    put("\x1b[31mz\x1b[0mw");
    check(attr(1, 2) == 0x14 && attr(1, 3) == 0x1E, "SGR 0 goes back to a new base colour");

    static const int vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    int all = 1;
    for (int i = 0; i < 8; i++) {
        char seq[32];
        snprintf(seq, sizeof seq, "\x1b[%d;%dm", 30 + i, 47 - i);
        put(seq);
        all &= vt.color == (vga[i] | vga[7 - i] << 4);
    }
    check(all, "the eight colours in ANSI order");

    // The cursor's visibility, and private sequences.
    start();
    put("\x1b[?25l");
    check(vt.cursor_hidden, "CSI ?25l hides the cursor");
    put("\x1b[?25h");
    check(!vt.cursor_hidden, "CSI ?25h shows it");
    put("\x1b[?1049h\x1b[?7l");
    check(at(0, 0) && blank_row(0), "other private sequences do nothing");

    // Sequences it does not know or cannot read.
    start();
    put("\x1b" "cA");
    check(!strcmp(line(0), "A"), "ESC c and the like are passed over");
    put("\x1b[5\x01" "B");
    check(!strcmp(line(0), "AB"), "a control character ends a broken sequence");
    put("\x1b[2X" "C");
    check(!strcmp(line(0), "ABC"), "an unknown final is passed over");
    put("\x1b[1;2;3;4;5;6;7;8;9;10;11;12m" "D");
    check(!strcmp(line(0), "ABCD"), "more parameters than it keeps");
    put("\x1b[99999999999999999999C" "E");
    check(ch(0, 9) == 'E', "an enormous number stays in range");
    start();
    put("\x1b[2" "\x80" "F");
    check(!strcmp(line(0), "F"), "a byte past ASCII ends a sequence");
    start();
    put("x\x07\x01\x7fy");
    check(!strcmp(line(0), "xy"), "other control characters are not shown");
    put("\x1b[1m\x1b");
    vt_reset(&vt);
    put("[31mq");
    check(!strcmp(line(0), "[31mq") && attr(0, 0) == C0 && at(0, 5), "vt_reset drops a half-read sequence and clears");

    // Erasing back, for line editing.
    start();
    put("abc");
    vt_erase_back(&vt);
    check(!strcmp(line(0), "ab") && at(0, 2), "erase back");
    start();
    put("0123456789");
    vt_erase_back(&vt);
    check(!strcmp(line(0), "012345678") && at(0, 9), "erase back on the last column");
    start();
    put("0123456789x");
    vt_erase_back(&vt);
    vt_erase_back(&vt);
    check(blank_row(1) && !strcmp(line(0), "012345678") && at(0, 9), "erase back across a wrap");
    start();
    vt_erase_back(&vt);
    check(at(0, 0), "erase back at the top left");

    // vt_clear keeps the colour; vt_reset does not.
    start();
    put("\x1b[41mab");
    vt_clear(&vt);
    check(attr(2, 2) == 0x47 && at(0, 0), "vt_clear in the current colour");
    put("\x1b[?25l");
    vt_reset(&vt);
    check(attr(2, 2) == C0 && !vt.cursor_hidden, "vt_reset back to the base");
    check(cells[W * H] == 0xBEEF, "still nothing past the grid");

    printf("test_vt: %d of %d checks %s\n", checks - fails, checks, fails ? "passed, FAILURES above" : "passed");
    return fails != 0;
}

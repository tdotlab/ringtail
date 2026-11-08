// more [FILE...]    show text one screenful at a time
//
// Reads each FILE, or standard input when none is named (or for "-"),
// and waits for a key at the bottom of every screen: Space or PgDn
// for the next screen, Enter or Down for one more line, q to quit.
// For a plain file the prompt says how far through it you are.
//
// The keys come from the terminal itself (/dev/tty), not from standard
// input, so `cat notes.txt | more` works. When the output is not the
// screen, or there is no terminal to ask, more copies the text through
// like cat. Ctrl+C ends it.
//
// The terminal is raw only while the prompt waits for a key, and there
// Ctrl+C arrives as a key and means quit: a program killed while raw
// has the screen wiped, which would take the page you were reading
// with it. Between prompts the terminal stays cooked, so Ctrl+C still
// kills more, and the pipeline behind it, if the input has stalled.
//
// Counting screens means knowing what the console will do with each
// byte, so `model` follows its rules: 80 columns, tabs to the next
// multiple of 8, a line that fills the last column wrapping only when
// another character comes, and escape sequences taking no room.
#include "ulib.h"

#define COLS 80
#define PAGE 24              // the 25th row is the prompt's

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: more [FILE...]\n");
    return 2;
}

static int tty = -1;         // keys, when we are paging
static int paging;
static char ob[1024];
static uint32_t on;

static int col, pending, esc;  // the console, as far as we can tell
static uint32_t rows;          // lines started since the last prompt
static uint32_t allowance = PAGE;
static uint32_t shown, size;   // bytes of this file shown; its size, 0 if unknown

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(STDOUT_FILENO, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "more: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void push(char c) {
    if (on == sizeof ob) flush();
    ob[on++] = c;
}

static void pushs(const char* s) {
    for (; *s; s++) push(*s);
}

// What the console will do with c.
static void model(unsigned char c) {
    if (esc == 1) { esc = c == '[' ? 2 : 0; return; }
    if (esc == 2) {
        if (c >= 0x40 && c <= 0x7E) { esc = 0; pending = 0; }
        else if (c < 0x20 || c > 0x3F) esc = 0;
        return;
    }
    if (c == 0x1B) { esc = 1; return; }
    switch (c) {
    case '\n': pending = 0; col = 0; rows++; return;
    case '\r': pending = 0; col = 0; return;
    case '\b':
        if (pending) pending = 0;
        else if (col > 0) col--;
        else if (rows) { rows--; col = COLS - 1; }
        return;
    case '\t':
        if (pending) { pending = 0; col = 0; rows++; }
        col = (col + 8) & ~7;
        if (col >= COLS) { col = COLS - 1; pending = 1; }
        return;
    }
    if (c < 0x20) return;
    if (pending) { pending = 0; col = 0; rows++; }
    if (col == COLS - 1) pending = 1;
    else col++;
}

enum { K_NONE, K_LINE, K_PAGE, K_QUIT, K_FAIL };

static int key(void) {
    unsigned char c;
    if (sys_read(tty, &c, 1) <= 0) return K_FAIL;
    if (c == ' ') return K_PAGE;
    if (c == '\r' || c == '\n') return K_LINE;
    if (c == 'q' || c == 'Q' || c == 3) return K_QUIT;  // 3: Ctrl+C
    if (c != 0x1B) return K_NONE;
    // Arrow keys and PgDn arrive as ESC [ ...; a lone Esc is just that
    // byte, so give the rest of a sequence a moment to show up.
    pollfd_t p = { tty, POLLIN, 0 };
    if (sys_pollfd(&p, 1, 50) <= 0 || sys_read(tty, &c, 1) <= 0 || c != '[') return K_NONE;
    uint32_t param = 0;
    for (;;) {
        if (sys_pollfd(&p, 1, 50) <= 0 || sys_read(tty, &c, 1) <= 0) return K_NONE;
        if (c >= '0' && c <= '9') param = param * 10 + (uint32_t)(c - '0');
        else if (c == ';') param = 0;  // a modifier comes first: the key is what follows
        else break;
    }
    if (c == 'B') return K_LINE;
    if (c == '~' && param == 6) return K_PAGE;
    return K_NONE;
}

// The prompt, in the empty bottom row, and the answer to it.
static void prompt(void) {
    char msg[48];
    if (size) {
        uint32_t pct = size > 40000000u ? shown / (size / 100) : shown * 100 / size;
        snprintf(msg, sizeof msg, "\x1b[7m--More--(%u%%)\x1b[0m", pct);
    } else {
        snprintf(msg, sizeof msg, "\x1b[7m--More--\x1b[0m");
    }
    int prev = sys_tty_mode(TTY_RAW | TTY_NOSIG);
    if (prev < 0) {  // not the foreground job: nobody to ask
        paging = 0;
        return;
    }
    pushs(msg);
    flush();
    int k;
    do k = key(); while (k == K_NONE);
    sys_tty_mode(prev);
    pushs("\r\x1b[K");
    flush();
    col = 0;
    pending = 0;
    rows = 0;
    if (k == K_QUIT) sys_exit(0);
    if (k == K_FAIL) { paging = 0; return; }
    allowance = k == K_LINE ? 1 : PAGE;
}

static void put_byte(unsigned char c) {
    if (paging) {
        // A character that wraps goes on a new row at once, and that row
        // may be the prompt's: do the wrap ourselves, then ask.
        int prints = esc == 0 && (c == '\t' || (c >= 0x20));
        if (prints && pending && rows + 1 >= allowance) {
            push('\n');
            model('\n');
        }
        if (rows >= allowance) prompt();
        model(c);
    }
    push((char)c);
}

static void put_str(const char* s) {
    for (; *s; s++) put_byte((unsigned char)*s);
}

static void heading(const char* name, int first) {
    if (!first) put_byte('\n');
    put_str("==> ");
    put_str(name);
    put_str(" <==\n");
}

static int feed(int fd, const char* name) {
    char buf[512];
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            put_byte((unsigned char)buf[i]);
            shown++;
        }
        flush();
    }
    if (n < 0) {
        flush();
        dprintf(STDERR_FILENO, "more: %s: %s\n", name, strerror(n));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int a = 1;
    if (a < argc && !strcmp(argv[a], "--")) a++;
    for (int i = a; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1]) return usage();
    if (a == argc && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();  // nothing to show

    if (sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) {
        tty = sys_open(DEV_TTY, O_RDONLY);
        paging = tty >= 0;
    }

    int files = argc - a, bad = 0;
    for (int i = a; i < argc || (i == a && files == 0); i++) {
        int stdin_arg = !files || !strcmp(argv[i], "-");
        const char* name = stdin_arg ? "standard input" : argv[i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            flush();
            dprintf(STDERR_FILENO, "more: %s: %s\n", name, strerror(fd));
            bad = 1;
            continue;
        }
        shown = size = 0;
        if (sys_fdtype(fd) == FDTYPE_FILE) {  // how far along we are, as a percentage
            int end = sys_seek(fd, 0, SEEK_END);
            if (end > 0 && sys_seek(fd, 0, SEEK_SET) == 0) size = (uint32_t)end;
            else sys_seek(fd, 0, SEEK_SET);
        }
        if (files > 1) {
            heading(name, i == a);
        }
        bad |= feed(fd, name);
        if (!stdin_arg) sys_close(fd);
    }
    flush();
    sys_exit(bad);
    return bad;
}

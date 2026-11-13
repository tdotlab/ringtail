// unexpand [-a | --first-only] [-t LIST] [FILE...]
//
// Writes the FILEs (or standard input, or "-") with the runs of spaces
// and tabs that lead up to a tab stop turned into tabs. The stops are
// every 8 columns unless -t says otherwise. By default only the blanks at
// the start of a line are looked at.
//   -a           look at the blanks everywhere on the line
//   --first-only the default, and it wins over -a and over what -t implies
//   -t  LIST     where the stops are, as for expand (and it implies -a)
//
// A single space is turned into a tab only when it follows another blank
// or starts the line: "a b" stays as it is even if the space ends at a
// stop. A tab that is already there is kept. Past the last stop of a
// list that has no end (-t 4,8) nothing is changed any more on that
// line. The files are one stream, as for expand, and columns count bytes.
// A backspace goes back a column. A last line with no newline stays
// without one (on the screen it gets one, so the prompt starts clean). A
// file that cannot be read is reported and the rest still follow; the
// status is then 1.
#include "ulib.h"
#include "lineio.h"
#include "tabstops.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: unexpand [-a | --first-only] [-t LIST] [FILE...]\n");
    return 2;
}

static lio_out_t out;
static tabs_t tabs;
static int bad;

// The files, as one stream.
static char** files;
static int nfiles, next_file;
static lio_in_t in;
static int in_open, in_stdin;
static const char* in_name;

// The next byte of the files taken together, or -1 at the end of them.
static int next_byte(void) {
    for (;;) {
        if (!in_open) {
            if (next_file >= nfiles) return -1;
            in_name = files[next_file++];
            in_stdin = !strcmp(in_name, "-");
            if (in_stdin) {
                in_name = "standard input";
                lio_in_init(&in, STDIN_FILENO);
            } else {
                int fd = sys_open(in_name, O_RDONLY);
                if (fd < 0) {
                    lio_flush(&out);
                    fs_error("unexpand", in_name, fd);
                    bad = 1;
                    continue;
                }
                lio_in_init(&in, fd);
            }
            in_open = 1;
        }
        int c = lio_getc(&in, &out);
        if (c >= 0) return c;
        if (in.err) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "unexpand: %s: %s\n", in_name, strerror(in.err));
            bad = 1;
        }
        if (!in_stdin) sys_close(in.fd);
        in_open = 0;
    }
}

// Which long option s (without its dashes, maybe with =VALUE) is, shortened
// or not: 'a' (all), 'f' (first-only) or 't' (tabs), or 0.
static int long_option(const char* s, const char** arg) {
    static const char* names[] = { "all", "first-only", "tabs" };
    const char* eq = strchr(s, '=');
    uint32_t n = eq ? (uint32_t)(eq - s) : (uint32_t)strlen(s);
    if (!n) return 0;
    for (int i = 0; i < 3; i++) {
        if (n <= strlen(names[i]) && !strncmp(s, names[i], n)) {
            *arg = eq ? eq + 1 : 0;
            return names[i][0];
        }
    }
    return 0;
}

// Writes `pending` spaces that were held back, the first of them as a
// tab if it came just before a tab stop and others follow.
static void flush_pending(uint32_t pending, int first_is_tab) {
    for (uint32_t i = 0; i < pending; i++) lio_putc(&out, i == 0 && first_is_tab ? '\t' : ' ');
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0, all = 0, first_only = 0;
    tabs_init(&tabs);
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            argv[1 + nops++] = argv[a];
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (s[1] == '-') {
            const char* arg = 0;
            int which = long_option(s + 2, &arg);
            if (!which) return usage();
            if (which != 't') {
                if (arg) return usage();
                if (which == 'a') all = 1;
                else first_only = 1;
                continue;
            }
            if (!arg) {
                if (++a == argc) return usage();
                arg = argv[a];
            }
            if (tabs_add(&tabs, arg, "unexpand")) return 1;
            all = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'a') {
                all = 1;
            } else if (*o == 't') {
                const char* arg = o + 1;
                if (!*arg) {
                    if (++a == argc) return usage();
                    arg = argv[a];
                }
                if (tabs_add(&tabs, arg, "unexpand")) return 1;
                all = 1;
                break;
            } else {
                return usage();
            }
        }
    }
    if (tabs_finish(&tabs, "unexpand")) return 1;
    if (first_only) all = 0;
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    static char* just_stdin[] = { "-" };
    files = nops ? argv + 1 : just_stdin;
    nfiles = nops ? nops : 1;

    lio_out_init(&out, STDOUT_FILENO, "unexpand");
    int last_byte = '\n';

    // The state of one line.
    uint64_t col = 0;
    int convert = 1;
    int prev_blank = 1;  // the line starts as if after a blank
    uint32_t pending = 0;  // spaces held back, to be tabs or not
    int one_blank = 0;  // the first of them ended at a tab stop

    for (;;) {
        int c = next_byte();
        if (convert) {
            int blank = c == ' ' || c == '\t';
            if (blank) {
                int last;
                uint64_t to = tabs_next(&tabs, col, &last);
                if (last) {
                    convert = 0;  // no stop is left on this line
                } else {
                    if (to < col) {
                        lio_flush(&out);
                        dprintf(STDERR_FILENO, "unexpand: input line is too long\n");
                        return 1;
                    }
                    if (c == '\t') {
                        col = to;
                        // Whatever was held back is covered by this tab, bar
                        // a blank that ended at the stop before it.
                        if (pending && one_blank) lio_putc(&out, '\t');
                        pending = 0;
                    } else {
                        col++;
                        if (!(prev_blank && col == to)) {
                            // Not known yet whether these become a tab.
                            if (col == to) one_blank = 1;
                            pending++;
                            prev_blank = 1;
                            continue;
                        }
                        if (pending && one_blank) lio_putc(&out, '\t');
                        pending = 0;
                        c = '\t';
                    }
                }
            } else if (c == '\b') {
                if (col) col--;
            } else {
                col++;
            }
            if (pending) {
                flush_pending(pending, pending > 1 && one_blank);
                pending = 0;
            }
            one_blank = 0;
            prev_blank = blank;
            if (!all && !blank) convert = 0;
        }
        if (c < 0) break;
        lio_putc(&out, (char)c);
        last_byte = c;
        if (c == '\n') {
            col = 0;
            convert = 1;
            prev_blank = 1;
            pending = 0;
        }
    }
    if (last_byte != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) lio_putc(&out, '\n');
    lio_flush(&out);
    return bad;
}

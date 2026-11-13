// expand [-i] [-t LIST] [FILE...]
//
// Writes the FILEs (or standard input, or "-") with each tab turned into
// the spaces that reach the next tab stop. The stops are every 8 columns
// unless -t says otherwise. The files are one stream: a last line with no
// newline is carried on by the first line of the next file, and so is the
// column. Columns count bytes, not characters: a UTF-8 letter is as many
// columns as it has bytes, and nothing is a double-width. A backspace goes
// back a column (but not past the left edge); a carriage return does not
// start the column over.
//   -i  convert only the tabs that come before any other character
//       on a line (a backspace counts as another character)
//   -t  LIST: one number N, a tab stop at every N columns; or numbers
//       separated by commas or blanks, each past the last, which are the
//       stops themselves, after which a tab is a single space. The last may
//       start with / ("4,/8": then at every multiple of 8) or + ("4,+8":
//       then every 8 more than the last). Another -t goes on from the one
//       before. There is no -4 for -t 4.
// A last line with no newline stays without one (but on the screen it gets
// one, so the prompt starts clean). A file that cannot be read is reported
// and the rest still follow; the status is then 1.
#include "ulib.h"
#include "lineio.h"
#include "tabstops.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: expand [-i] [-t LIST] [FILE...]\n");
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
                    fs_error("expand", in_name, fd);
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
            dprintf(STDERR_FILENO, "expand: %s: %s\n", in_name, strerror(in.err));
            bad = 1;
        }
        if (!in_stdin) sys_close(in.fd);
        in_open = 0;
    }
}

static const char* long_option(const char* s, const char** arg) {
    static const char* names[] = { "initial", "tabs" };
    const char* eq = strchr(s, '=');
    uint32_t n = eq ? (uint32_t)(eq - s) : (uint32_t)strlen(s);
    if (!n) return 0;
    for (int i = 0; i < 2; i++) {
        if (n <= strlen(names[i]) && !strncmp(s, names[i], n)) {
            *arg = eq ? eq + 1 : 0;
            return names[i];
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0, initial = 0;
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
            const char* name = long_option(s + 2, &arg);
            if (!name) return usage();
            if (name[0] == 'i') {
                if (arg) return usage();
                initial = 1;
                continue;
            }
            if (!arg) {
                if (++a == argc) return usage();
                arg = argv[a];
            }
            if (tabs_add(&tabs, arg, "expand")) return 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'i') {
                initial = 1;
            } else if (*o == 't') {
                const char* arg = o + 1;
                if (!*arg) {
                    if (++a == argc) return usage();
                    arg = argv[a];
                }
                if (tabs_add(&tabs, arg, "expand")) return 1;
                break;
            } else {
                return usage();
            }
        }
    }
    if (tabs_finish(&tabs, "expand")) return 1;
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    static char* just_stdin[] = { "-" };
    files = nops ? argv + 1 : just_stdin;
    nfiles = nops ? nops : 1;

    lio_out_init(&out, STDOUT_FILENO, "expand");
    int last_byte = '\n';
    uint64_t col = 0;
    int convert = 1;
    for (;;) {
        int c = next_byte();
        if (c < 0) break;
        if (convert) {
            if (c == '\t') {
                int last;
                uint64_t to = tabs_next(&tabs, col, &last);
                if (last) to = col + 1;
                if (to < col) {
                    lio_flush(&out);
                    dprintf(STDERR_FILENO, "expand: input line is too long\n");
                    return 1;
                }
                while (++col < to) lio_putc(&out, ' ');
                c = ' ';
            } else if (c == '\b') {
                if (col) col--;
            } else {
                col++;
            }
            if (initial && c != ' ' && c != '\t') convert = 0;
        }
        lio_putc(&out, (char)c);
        last_byte = c;
        if (c == '\n') {
            col = 0;
            convert = 1;
        }
    }
    if (last_byte != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) lio_putc(&out, '\n');
    lio_flush(&out);
    return bad;
}

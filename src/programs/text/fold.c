// fold [-b] [-s] [-w WIDTH] [FILE...]
//
// Writes the FILEs (or standard input, or "-") with every line cut so it
// is no more than WIDTH columns, 80 unless -w says otherwise. The cut
// parts follow each other as lines of their own. Each file is folded by
// itself. A tab goes on to the next multiple of 8, a backspace goes back a
// column (but not past the left edge), a carriage return goes to column 0,
// and every other byte is one column: this works on bytes, so a UTF-8
// letter counts as many columns as it has bytes. A tab, or any one byte,
// that is wider than the whole width gets a line to itself.
//   -b  count bytes: a tab, a backspace and a carriage return are one
//       column each as well
//   -s  cut after the last blank (space or tab) that fits instead of in the
//       middle of a word; a line with no blank in it is cut at WIDTH as usual
//   -w  WIDTH, a whole number of 1 or more. -N is the same as -w N.
// A last line with no newline stays without one (but on the screen it gets
// one, so the prompt starts clean). A file that cannot be read is reported
// and the rest still follow; the status is then 1.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: fold [-b] [-s] [-w WIDTH] [FILE...]\n");
    return 2;
}

#define WIDTH_MAX 0xfffffffffffffff6ull

static lio_out_t out;
static uint64_t width = 80;
static int count_bytes, break_spaces;
static int bad;
static int last_byte = '\n';

// Said as GNU fold says it, reasons and all. Returns 0, or 1 once it has
// told the user what is wrong with s.
static int parse_width(const char* s) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    int overflow = 0;
    uint64_t v = 0;
    const char* digits = p;
    for (; *p >= '0' && *p <= '9'; p++) {
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) overflow = 1;
        else v = v * 10 + d;
    }
    if (p == digits || *p) {
        dprintf(STDERR_FILENO, "fold: invalid number of columns: '%s'\n", s);
        return 1;
    }
    if (overflow || v > WIDTH_MAX) {
        dprintf(STDERR_FILENO, "fold: invalid number of columns: '%s': Value too large for defined data type\n", s);
        return 1;
    }
    if (!v) {
        dprintf(STDERR_FILENO, "fold: invalid number of columns: '%s': Numerical result out of range\n", s);
        return 1;
    }
    width = v;
    return 0;
}

// Where a column is after c.
static uint64_t adjust(uint64_t col, int c) {
    if (count_bytes) return col + 1;
    if (c == '\b') return col ? col - 1 : 0;
    if (c == '\r') return 0;
    if (c == '\t') return col + 8 - col % 8;
    return col + 1;
}

static void put(const char* p, uint32_t n) {
    if (!n) return;
    last_byte = (unsigned char)p[n - 1];
    lio_put(&out, p, n);
}

static lio_line_t line;  // what has been read of this output line

static void fold_fd(int fd, const char* name) {
    lio_in_t in;
    lio_in_init(&in, fd);
    uint64_t col = 0;
    int c;
    while ((c = lio_getc(&in, &out)) >= 0) {
        if (c == '\n') {
            lio_line_addc(&line, '\n', "fold");
            put(line.s, line.n);
            line.n = 0;
            col = 0;
            continue;
        }
    rescan:
        col = adjust(col, c);
        if (col > width) {
            // This byte would make the line too long.
            if (break_spaces) {
                uint32_t end = line.n;
                while (end) {
                    end--;
                    if (line.s[end] == ' ' || line.s[end] == '\t') {
                        end++;
                        break;
                    }
                }
                if (end) {
                    // Write the line up to the last blank, and carry on with
                    // what came after it.
                    put(line.s, end);
                    put("\n", 1);
                    memmove(line.s, line.s + end, line.n - end);
                    line.n -= end;
                    col = 0;
                    for (uint32_t i = 0; i < line.n; i++) col = adjust(col, (unsigned char)line.s[i]);
                    goto rescan;
                }
            }
            if (!line.n) {
                lio_line_addc(&line, (char)c, "fold");
                continue;
            }
            lio_line_addc(&line, '\n', "fold");
            put(line.s, line.n);
            line.n = 0;
            col = 0;
            goto rescan;
        }
        lio_line_addc(&line, (char)c, "fold");
    }
    put(line.s, line.n);
    line.n = 0;
    if (in.err) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "fold: %s: %s\n", name, strerror(in.err));
        bad = 1;
    }
}

// Whether s (without its dashes, maybe with =VALUE) is a long option, whole
// or shortened: 'b', 's' or 'w'; 0 if not.
static int long_option(const char* s, const char** arg) {
    static const char* names[] = { "bytes", "spaces", "width" };
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

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0;
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
            if (which == 'w') {
                if (!arg) {
                    if (++a == argc) return usage();
                    arg = argv[a];
                }
                if (parse_width(arg)) return 1;
                continue;
            }
            if (arg) return usage();
            if (which == 'b') count_bytes = 1;
            else break_spaces = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'b') {
                count_bytes = 1;
            } else if (*o == 's') {
                break_spaces = 1;
            } else if (*o == 'w') {
                const char* arg = o + 1;
                if (!*arg) {
                    if (++a == argc) return usage();
                    arg = argv[a];
                }
                if (parse_width(arg)) return 1;
                break;
            } else if (*o >= '0' && *o <= '9') {
                // -5 is -w 5; and, as in GNU fold, the rest of the argument
                // is part of the number, so -5s is a bad number.
                if (parse_width(o)) return 1;
                break;
            } else {
                return usage();
            }
        }
    }
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    lio_out_init(&out, STDOUT_FILENO, "fold");
    line = (lio_line_t){ 0, 0, 0, 0 };
    for (int i = 0; i < nops || (!nops && !i); i++) {
        int stdin_arg = !nops || !strcmp(argv[1 + i], "-");
        const char* name = stdin_arg ? "standard input" : argv[1 + i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (fd < 0) {
            lio_flush(&out);
            fs_error("fold", name, fd);
            bad = 1;
            continue;
        }
        fold_fd(fd, name);
        if (!stdin_arg) sys_close(fd);
    }
    if (last_byte != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) lio_putc(&out, '\n');
    lio_flush(&out);
    return bad;
}

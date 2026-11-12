// nl [-b STYLE] [-h STYLE] [-f STYLE] [-n FORMAT] [-w WIDTH] [-s SEP]
//    [-v FIRST] [-i INCR] [-l NUM] [-d CC] [-p] [FILE...]
//
// Writes each FILE (or standard input, or "-") with its lines numbered,
// counting on from one file to the next. A line is numbered, then the
// separator, then the line. Each option takes its value either joined
// to it (-w3) or as the next argument (-w 3), and options may come after
// the files; "--" ends them.
//
//   -b STYLE  which lines in the body are numbered: a (all), t (only
//             those with something in them, the default), n (none), or
//             pREGEX (those where the basic-syntax REGEX matches)
//   -h, -f    the same for headers (default n) and footers (default n)
//   -n FORMAT ln (left, no zeros), rn (right, no zeros; the default) or
//             rz (right, with zeros)
//   -w WIDTH  at least this many columns for the number (default 6; a
//             longer number just takes more)
//   -s SEP    what goes between the number and the line (default a tab)
//   -v FIRST  the number the first line gets (default 1)
//   -i INCR   what to add for each numbered line (default 1)
//   -l NUM    count NUM blank lines in a row as one, when numbering all
//             lines (-b a): only the NUMth of a run gets a number
//   -d CC     the string that marks the pages (default \:; one
//             character gets a ':' after it)
//   -p        do not start the numbers again on each page
//
// A line made only of \:\:\: starts a page's header, \:\:\: its body and
// \: its footer (with -d, its string repeated three times, twice and
// once). Such a line is written as an empty one, and the numbers start
// again at -v unless -p is given.
// The text before the first one is a body. A line that is not numbered
// has as many spaces where the number would be as the number and the
// separator take. A last line with no newline gets one.
//
// Numbers are 64 bits. If they would run out, nl stops with an error
// when it comes to number another line.
#include "ulib.h"
#include "lineio.h"
#include "regex.h"

static int usage(void) {
    dprintf(STDERR_FILENO,
            "usage: nl [-b STYLE] [-h STYLE] [-f STYLE] [-n ln|rn|rz] [-w WIDTH] [-s SEP]\n"
            "          [-v FIRST] [-i INCR] [-l NUM] [-d CC] [-p] [FILE...]\n");
    return 2;
}

typedef struct {
    char kind;  // 'a', 't', 'n' or 'p'
    re_t re;    // for 'p'
} style_t;

enum { HEADER, BODY, FOOTER };

static style_t styles[3] = { { 'n', {0} }, { 't', {0} }, { 'n', {0} } };
static enum { LN, RN, RZ } format = RN;
static int64_t width = 6, first = 1, incr = 1, join = 1;
static const char* sep = "\t";
static uint32_t seplen = 1;
static const char* delim = "\\:";
static char delim1[3];  // a one-character delimiter with its colon
static uint32_t dlen = 2;
static int renumber = 1;

static lio_out_t out;

static void die(const char* fmt, const char* a) {
    lio_flush(&out);
    dprintf(STDERR_FILENO, "nl: ");
    dprintf(STDERR_FILENO, fmt, a);
    dprintf(STDERR_FILENO, "\n");
    sys_exit(1);
}

// A decimal number the way strtoimax reads it: blanks, a sign, digits
// and nothing more. 0, or -1 for anything else or a number too big.
static int parse_i64(const char* s, int64_t* v) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (*s < '0' || *s > '9') return -1;
    uint64_t limit = neg ? 0x8000000000000000ull : 0x7fffffffffffffffull, n = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        unsigned d = (unsigned)(*s - '0');
        if (n > (limit - d) / 10) return -1;
        n = n * 10 + d;
    }
    if (*s) return -1;
    *v = neg ? (int64_t)(0 - n) : (int64_t)n;
    return 0;
}

// A number in 1..INT32_MAX, or an error message built from `fmt`.
static int64_t positive(const char* v, const char* fmt) {
    int64_t n;
    if (parse_i64(v, &n) || n < 1 || n > 0x7fffffff) die(fmt, v);
    return n;
}

static void set_style(style_t* st, const char* v, const char* what) {
    if (v[0] == 'a' || v[0] == 't' || v[0] == 'n') {
        st->kind = v[0];
    } else if (v[0] == 'p') {
        const char* err = re_compile(&st->re, v + 1, (uint32_t)strlen(v + 1), 0);
        if (err) die("invalid regular expression: %s", err);
        st->kind = 'p';
    } else {
        die(what, v);
    }
}

static void spaces(int64_t n) {
    static const char sp[64] = "                                                                ";
    while (n > 0) {
        uint32_t take = n > 64 ? 64 : (uint32_t)n;
        lio_put(&out, sp, take);
        n -= take;
    }
}

static void zeros(int64_t n) {
    static const char z[64] = "0000000000000000000000000000000000000000000000000000000000000000";
    while (n > 0) {
        uint32_t take = n > 64 ? 64 : (uint32_t)n;
        lio_put(&out, z, take);
        n -= take;
    }
}

static void put_number(int64_t n) {
    char d[24];
    int nd = 0, neg = n < 0;
    uint64_t u = neg ? 0 - (uint64_t)n : (uint64_t)n;
    do {
        d[nd++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    int64_t pad = width - nd - neg;
    if (pad < 0) pad = 0;
    if (format == RN) spaces(pad);
    if (neg) lio_putc(&out, '-');
    if (format == RZ) zeros(pad);
    while (nd) lio_putc(&out, d[--nd]);
    if (format == LN) spaces(pad);
}

// Whether the line is one of the page markers: the delimiter once,
// twice or three times. Gives 1 to 3, or 0.
static int marker(const char* s, uint32_t n) {
    if (!dlen || n % dlen || n / dlen < 1 || n / dlen > 3) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (s[i] != delim[i % dlen]) return 0;
    return (int)(n / dlen);
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
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'p') {
                renumber = 0;
                continue;
            }
            if (!strchr("bdfhilnsvw", *o)) return usage();
            const char* v = o + 1;
            if (!*v) {
                if (++a == argc) return usage();
                v = argv[a];
            }
            switch (*o) {
            case 'b': set_style(&styles[BODY], v, "invalid body numbering style: '%s'"); break;
            case 'h': set_style(&styles[HEADER], v, "invalid header numbering style: '%s'"); break;
            case 'f': set_style(&styles[FOOTER], v, "invalid footer numbering style: '%s'"); break;
            case 'd':
                dlen = (uint32_t)strlen(v);
                delim = v;
                if (dlen == 1) {  // "@" means "@:"
                    delim1[0] = v[0];
                    delim1[1] = ':';
                    delim = delim1;
                    dlen = 2;
                }
                break;
            case 'i':
                if (parse_i64(v, &incr)) die("invalid line number increment: '%s'", v);
                break;
            case 'v':
                if (parse_i64(v, &first)) die("invalid starting line number: '%s'", v);
                break;
            case 'l': join = positive(v, "invalid line number of blank lines: '%s'"); break;
            case 'w': width = positive(v, "invalid line number field width: '%s'"); break;
            case 's':
                sep = v;
                seplen = (uint32_t)strlen(v);
                break;
            case 'n':
                if (!strcmp(v, "ln")) format = LN;
                else if (!strcmp(v, "rn")) format = RN;
                else if (!strcmp(v, "rz")) format = RZ;
                else die("invalid line numbering format: '%s'", v);
                break;
            }
            break;
        }
    }
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    lio_out_init(&out, STDOUT_FILENO, "nl");
    int bad = 0, section = BODY, overflow = 0;
    int64_t lineno = first, blanks = 0;
    lio_line_t l = { 0, 0, 0, 0 };
    for (int i = 0; i < nops || i == 0; i++) {
        int stdin_arg = !nops || !strcmp(argv[1 + i], "-");
        const char* name = stdin_arg ? "standard input" : argv[1 + i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (fd < 0) {
            fs_error("nl", name, fd);
            bad = 1;
            continue;
        }
        lio_in_t in;
        lio_in_init(&in, fd);
        while (lio_getline(&in, &out, &l)) {
            int m = marker(l.s, l.n);
            if (m) {
                section = m == 3 ? HEADER : m == 2 ? BODY : FOOTER;
                if (renumber) {
                    lineno = first;
                    overflow = 0;
                }
                lio_putc(&out, '\n');
                continue;
            }
            style_t* st = &styles[section];
            int number = 0;
            if (st->kind == 'a') {
                number = l.n || join < 2 || ++blanks == join;
                if (number) blanks = 0;
            } else if (st->kind == 't') {
                number = l.n > 0;
            } else if (st->kind == 'p') {
                number = re_test(&st->re, l.s, l.n);
            }
            if (number) {
                if (overflow) die("line number overflow", "");
                put_number(lineno);
                lio_put(&out, sep, seplen);
                overflow = __builtin_add_overflow(lineno, incr, &lineno);
            } else {
                spaces(width + seplen);
            }
            lio_put(&out, l.s, l.n);
            lio_putc(&out, '\n');
        }
        if (in.err) {
            dprintf(STDERR_FILENO, "nl: %s: %s\n", name, strerror(in.err));
            bad = 1;
        }
        if (!stdin_arg) sys_close(fd);
    }
    lio_flush(&out);
    return bad;
}

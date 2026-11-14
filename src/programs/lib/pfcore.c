#include "pfcore.h"

#define INT_MAX_ 2147483647
#define INT_MIN_ (-2147483647 - 1)

typedef struct {
    const pf_io_t* io;
    int status;  // 0, or 1 once a message said something was wrong
    int done;    // the run must end now: a fatal error, or a "\c"
    int werr;    // a conversion was too big to write: said at the very end, as a closed stdout is
    int argc;    // the arguments not used yet
    char* const* argv;
} pf_t;

// One directive's flags, width and precision.
typedef struct {
    int minus, plus, space, hash, zero;
    uint64_t width;
    int64_t prec;  // -1 when there is none
    int over;      // a width or precision too large to write
    int intmin;    // the width is INT_MIN from a *: unwritable, but only when something is written
} dir_t;

static uint32_t slen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(pf_t* p, const char* s, uint32_t n) {
    if (n) p->io->out(p->io->ctx, s, n);
}

static void putc_(pf_t* p, char c) {
    put(p, &c, 1);
}

static void fill(pf_t* p, char c, uint64_t n) {
    char buf[64];
    for (int i = 0; i < 64; i++) buf[i] = c;
    while (n) {
        uint32_t k = n > 64 ? 64 : (uint32_t)n;
        put(p, buf, k);
        n -= k;
    }
}

static void say(pf_t* p, const char* s) {
    p->io->err(p->io->ctx, s, slen(s));
}

// s as GNU's quote() writes it in the C locale: in single quotes, with a
// backslash before \ and ', \a \b \t \n \v \f \r, and the other control and
// high bytes as three octal digits.
static void say_quoted(pf_t* p, const char* s) {
    say(p, "'");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char e[4];
        const char* t = 0;
        switch (c) {
        case 7: t = "\\a"; break;
        case 8: t = "\\b"; break;
        case 9: t = "\\t"; break;
        case 10: t = "\\n"; break;
        case 11: t = "\\v"; break;
        case 12: t = "\\f"; break;
        case 13: t = "\\r"; break;
        case '\\': t = "\\\\"; break;
        case '\'': t = "\\'"; break;
        default:
            if (c < 32 || c >= 127) {
                e[0] = '\\';
                e[1] = (char)('0' + (c >> 6));
                e[2] = (char)('0' + ((c >> 3) & 7));
                e[3] = (char)('0' + (c & 7));
                p->io->err(p->io->ctx, e, 4);
            } else {
                p->io->err(p->io->ctx, s, 1);
            }
        }
        if (t) say(p, t);
    }
    say(p, "'");
}

static void fatal(pf_t* p) {
    p->done = 1;
    p->status = 1;
}

static void write_error(pf_t* p) {
    p->werr = 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_space(char c) {
    return c == ' ' || (c >= 9 && c <= 13);
}

// ---------------------------------------------------------------- numbers

// What C's strtoimax/strtoumax with base 0 make of s: *end is where the number
// stops (s itself when there is none), *range says it did not fit. A leading
// 0x, 0b or 0 gives hexadecimal, binary or octal.
static uint64_t scan_number(const char* s, int is_signed, const char** end, int* range) {
    const char* q = s;
    *end = s;
    *range = 0;
    while (is_space(*q)) q++;
    int neg = 0;
    if (*q == '+' || *q == '-') neg = *q++ == '-';
    unsigned base = 10;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X') && hexval(q[2]) >= 0) {
        base = 16;
        q += 2;
    } else if (q[0] == '0' && (q[1] == 'b' || q[1] == 'B') && (q[2] == '0' || q[2] == '1')) {
        base = 2;
        q += 2;
    } else if (q[0] == '0') {
        base = 8;
    }
    uint64_t v = 0;
    int any = 0, over = 0;
    for (;; q++) {
        int d = hexval(*q);
        if (d < 0 || (unsigned)d >= base) break;
        any = 1;
        if (v > (UINT64_MAX - (uint64_t)d) / base) over = 1;
        else v = v * base + (uint64_t)d;
    }
    if (!any) return 0;
    *end = q;
    if (is_signed) {
        uint64_t limit = neg ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1;
        if (over || v > limit) {
            *range = 1;
            return neg ? (uint64_t)1 << 63 : limit;  // INT64_MIN's bits, or INT64_MAX
        }
        return neg ? (uint64_t)0 - v : v;
    }
    if (over) {
        *range = 1;
        return UINT64_MAX;
    }
    return neg ? (uint64_t)0 - v : v;
}

// An argument as a number, as GNU printf reads it: 'c or "c is the code of c
// (with a warning if more follows), anything else is a C number. A bad number
// says so and makes the status 1; what could be read is still used.
static uint64_t arg_number(pf_t* p, const char* s, int is_signed) {
    if ((s[0] == '"' || s[0] == '\'') && s[1]) {
        if (s[2]) {
            say(p, "printf: warning: ");
            say(p, s + 2);
            say(p, ": character(s) following character constant have been ignored\n");
        }
        return (unsigned char)s[1];
    }
    const char* end;
    int range;
    uint64_t v = scan_number(s, is_signed, &end, &range);
    if (range) {
        say(p, "printf: ");
        say_quoted(p, s);
        say(p, ": Numerical result out of range\n");
        p->status = 1;
    } else if (*end) {
        say(p, "printf: ");
        say_quoted(p, s);
        say(p, end == s ? ": expected a numeric value\n" : ": value not completely converted\n");
        p->status = 1;
    }
    return v;
}

// ---------------------------------------------------------------- escapes

static int is_odigit(char c) {
    return c >= '0' && c <= '7';
}

// The escape that starts at esc (a backslash). Returns how many characters
// after the backslash it took. octal_0: in %b, where \0NNN is the octal form.
static uint32_t print_esc(pf_t* p, const char* esc, int octal_0) {
    const char* q = esc + 1;
    if (*q == 'x') {
        int v = 0, n = 0;
        for (q++; n < 2 && hexval(*q) >= 0; n++, q++) v = v * 16 + hexval(*q);
        if (n == 0) {
            say(p, "printf: missing hexadecimal number in escape\n");
            fatal(p);
            return (uint32_t)(q - esc - 1);
        }
        putc_(p, (char)v);
    } else if (is_odigit(*q)) {
        int v = 0, n = 0;
        if (octal_0 && *q == '0') q++;
        for (; n < 3 && is_odigit(*q); n++, q++) v = v * 8 + (*q - '0');
        putc_(p, (char)v);
    } else if (*q == 'u' || *q == 'U') {
        char kind = *q;
        int digits = kind == 'u' ? 4 : 8;
        uint32_t v = 0;
        for (q++; digits > 0; digits--, q++) {
            if (hexval(*q) < 0) {
                say(p, "printf: missing hexadecimal number in escape\n");
                fatal(p);
                return (uint32_t)(q - esc - 1);
            }
            v = v * 16 + (uint32_t)hexval(*q);
        }
        if (v >= 0xd800 && v <= 0xdfff) {
            char t[12];
            int w = kind == 'u' ? 4 : 8;
            say(p, "printf: invalid universal character name \\");
            t[0] = kind;
            p->io->err(p->io->ctx, t, 1);
            for (int i = 0; i < w; i++) t[i] = "0123456789abcdef"[(v >> (4 * (w - 1 - i))) & 15];
            p->io->err(p->io->ctx, t, (uint32_t)w);
            say(p, "\n");
            fatal(p);
        } else if (v < 0x80) {
            putc_(p, (char)v);
        } else {
            // There is no character set here but ASCII: a character that is
            // not in it is written the way it was asked for.
            char t[12];
            int w = v > 0xffff ? 8 : 4;
            putc_(p, '\\');
            putc_(p, w == 8 ? 'U' : 'u');
            for (int i = 0; i < w; i++) t[i] = "0123456789ABCDEF"[(v >> (4 * (w - 1 - i))) & 15];
            put(p, t, (uint32_t)w);
        }
    } else if (*q == 'a') { putc_(p, 7); q++;
    } else if (*q == 'b') { putc_(p, 8); q++;
    } else if (*q == 'c') {
        p->status = 0;  // "\c" stops the run, and the run is a success
        p->done = 1;
        q++;
    } else if (*q == 'e') { putc_(p, 27); q++;
    } else if (*q == 'f') { putc_(p, 12); q++;
    } else if (*q == 'n') { putc_(p, 10); q++;
    } else if (*q == 'r') { putc_(p, 13); q++;
    } else if (*q == 't') { putc_(p, 9); q++;
    } else if (*q == 'v') { putc_(p, 11); q++;
    } else if (*q == '"' || *q == '\\') { putc_(p, *q); q++;
    } else {
        putc_(p, '\\');
        if (*q) {
            putc_(p, *q);
            q++;
        }
    }
    return (uint32_t)(q - esc - 1);
}

// %b: the argument with its escapes read.
static void print_esc_string(pf_t* p, const char* s) {
    for (; *s && !p->done; s++) {
        if (*s == '\\') s += print_esc(p, s, 1);
        else putc_(p, *s);
    }
}

// ----------------------------------------------------------- conversions

static void print_int(pf_t* p, int conv, uint64_t mag, int neg, const dir_t* d) {
    char digs[24];
    int nd = 0;
    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
    const char* alpha = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t v = mag;
    do {
        digs[nd++] = alpha[v % base];
        v /= base;
    } while (v);
    if (d->prec == 0 && mag == 0) nd = 0;
    uint64_t zeros = d->prec > nd ? (uint64_t)d->prec - (uint64_t)nd : 0;
    if (d->hash && conv == 'o' && zeros == 0 && (nd == 0 || digs[nd - 1] != '0')) zeros = 1;
    char sign = 0;
    if (conv == 'd' || conv == 'i') sign = neg ? '-' : d->plus ? '+' : d->space ? ' ' : 0;
    const char* prefix = "";
    uint32_t plen = 0;
    if (d->hash && mag != 0 && (conv == 'x' || conv == 'X')) {
        prefix = conv == 'x' ? "0x" : "0X";
        plen = 2;
    }
    uint64_t total = (sign ? 1u : 0u) + plen + zeros + (uint64_t)nd;
    uint64_t len = total > d->width ? total : d->width;
    if (d->over || len >= INT_MAX_ || (d->intmin && total > 0)) {
        write_error(p);
        return;
    }
    uint64_t padn = d->width > total ? d->width - total : 0;
    int zero_pad = d->zero && !d->minus && d->prec < 0;
    if (!d->minus && !zero_pad) fill(p, ' ', padn);
    if (sign) putc_(p, sign);
    put(p, prefix, plen);
    if (zero_pad) zeros += padn;
    fill(p, '0', zeros);
    while (nd) putc_(p, digs[--nd]);
    if (d->minus) fill(p, ' ', padn);
}

// %s (n bytes of s, cut by the precision) and %c (one byte).
static void print_bytes(pf_t* p, const char* s, uint64_t n, const dir_t* d) {
    uint64_t len = n > d->width ? n : d->width;
    if (d->over || len >= INT_MAX_ || (d->intmin && n > 0)) {
        write_error(p);
        return;
    }
    uint64_t padn = d->width > n ? d->width - n : 0;
    if (!d->minus) fill(p, ' ', padn);
    put(p, s, (uint32_t)n);
    if (d->minus) fill(p, ' ', padn);
}

// The next argument, or "" when they have run out.
static const char* next_arg(pf_t* p) {
    if (p->argc <= 0) return "";
    p->argc--;
    return *p->argv++;
}

// The directive that starts at f (a "%"). Returns its last character.
static const char* directive(pf_t* p, const char* f) {
    const char* start = f++;
    if (*f == '%') {
        putc_(p, '%');
        return f;
    }
    if (*f == 'b') {
        if (p->argc > 0) {
            const char* a = next_arg(p);
            print_esc_string(p, a);
        }
        return f;
    }
    if (*f == 'q') {
        say(p, "printf: %q: not supported\n");
        fatal(p);
        return f;
    }
    char ok[256];
    for (int i = 0; i < 256; i++) ok[i] = 0;
    for (const char* c = "aAcdeEfFgGiosuxX"; *c; c++) ok[(unsigned char)*c] = 1;
    dir_t d = {0, 0, 0, 0, 0, 0, -1, 0, 0};
    for (;; f++) {
        if (*f == '\'' || *f == 'I') {
            ok['a'] = ok['A'] = ok['c'] = ok['e'] = ok['E'] = ok['o'] = ok['s'] = ok['x'] = ok['X'] = 0;
        } else if (*f == '-') {
            d.minus = 1;
        } else if (*f == '+') {
            d.plus = 1;
        } else if (*f == ' ') {
            d.space = 1;
        } else if (*f == '#') {
            d.hash = 1;
            ok['c'] = ok['d'] = ok['i'] = ok['s'] = ok['u'] = 0;
        } else if (*f == '0') {
            d.zero = 1;
            ok['c'] = ok['s'] = 0;
        } else {
            break;
        }
    }
    if (*f == '*') {
        f++;
        int64_t w = 0;
        if (p->argc > 0) {
            const char* a = next_arg(p);
            w = (int64_t)arg_number(p, a, 1);
            if (w < INT_MIN_ || w > INT_MAX_) {
                say(p, "printf: invalid field width: ");
                say_quoted(p, a);
                say(p, "\n");
                fatal(p);
                return f;
            }
        }
        if (w < 0) {
            d.minus = 1;
            w = -w;
        }
        if (w > INT_MAX_) {
            d.intmin = 1;
            w = 0;
        }
        d.width = (uint64_t)w;
    } else {
        uint64_t w = 0;
        for (; *f >= '0' && *f <= '9'; f++) {
            w = w * 10 + (uint64_t)(*f - '0');
            if (w > INT_MAX_) w = INT_MAX_;
        }
        d.width = w;  // a width of INT_MAX or more is left at INT_MAX: print_int and print_bytes see it as too big
    }
    if (*f == '.') {
        f++;
        ok['c'] = 0;
        if (*f == '*') {
            f++;
            int64_t pr = 0;
            if (p->argc > 0) {
                const char* a = next_arg(p);
                pr = (int64_t)arg_number(p, a, 1);
                if (pr > INT_MAX_) {
                    say(p, "printf: invalid precision: ");
                    say_quoted(p, a);
                    say(p, "\n");
                    fatal(p);
                    return f;
                }
                if (pr < 0) pr = -1;
            }
            d.prec = pr;
        } else {
            uint64_t pr = 0;
            for (; *f >= '0' && *f <= '9'; f++) {
                pr = pr * 10 + (uint64_t)(*f - '0');
                if (pr > (uint64_t)INT_MAX_ + 1) pr = (uint64_t)INT_MAX_ + 1;
            }
            if (pr > INT_MAX_) d.over = 1;
            d.prec = (int64_t)pr;
        }
    }
    while (*f == 'l' || *f == 'L' || *f == 'h' || *f == 'j' || *f == 't' || *f == 'z') f++;
    unsigned char conv = (unsigned char)*f;
    if (!ok[conv]) {
        say(p, "printf: ");
        for (const char* c = start; c <= f && *c; c++) p->io->err(p->io->ctx, c, 1);
        say(p, ": invalid conversion specification\n");
        fatal(p);
        return f;
    }
    if (conv == 'a' || conv == 'A' || conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G') {
        say(p, "printf: ");
        for (const char* c = start; c <= f; c++) p->io->err(p->io->ctx, c, 1);
        say(p, ": floating point is not supported\n");
        fatal(p);
        return f;
    }
    const char* a = next_arg(p);
    if (conv == 'd' || conv == 'i') {
        int64_t v = (int64_t)arg_number(p, a, 1);
        print_int(p, conv, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0, &d);
    } else if (conv == 'o' || conv == 'u' || conv == 'x' || conv == 'X') {
        print_int(p, conv, arg_number(p, a, 0), 0, &d);
    } else if (conv == 'c') {
        print_bytes(p, a, 1, &d);  // an empty argument is a NUL
    } else {
        uint64_t n = 0;
        while (a[n] && (d.prec < 0 || (int64_t)n < d.prec)) n++;
        print_bytes(p, a, n, &d);
    }
    return f;
}

// One time through the format. Returns how many arguments it used.
static int pass(pf_t* p, const char* format) {
    int save = p->argc;
    for (const char* f = format; !p->done && *f; f++) {
        if (*f == '%') f = directive(p, f);
        else if (*f == '\\') f += print_esc(p, f, 0);
        else putc_(p, *f);
    }
    return save - p->argc;
}

int pf_run(const pf_io_t* io, const char* format, int nargs, char* const* args) {
    pf_t p = {io, 0, 0, 0, nargs, args};
    int used;
    do {
        used = pass(&p, format);
    } while (!p.done && used > 0 && p.argc > 0);
    if (!p.done && p.argc > 0) {
        say(&p, "printf: warning: ignoring excess arguments, starting with ");
        say_quoted(&p, *p.argv);
        say(&p, "\n");
    }
    if (p.werr) {
        say(&p, "printf: write error\n");
        p.status = 1;
    }
    return p.status;
}

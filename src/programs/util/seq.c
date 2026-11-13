// seq [-w] [-s SEP] [FIRST [INCR]] LAST
//
// Prints the numbers from FIRST (default 1) to LAST in steps of INCR
// (default 1: counting down needs a negative INCR), one per line. Numbers may have decimals (as many as 9): the output then
// has as many places as the most precise of FIRST and INCR.
//   -s SEP  put SEP between numbers instead of a newline (the last number
//           still ends with one)
//   -w      pad with zeros to the same width
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: seq [-w] [-s SEP] [FIRST [INCR]] LAST\n");
    return 2;
}

typedef struct {
    int64_t v;  // value times 10^prec
    int prec;
    int width;  // as written: sign, whole-number digits (leading zeros too), no places
} num_t;

static int64_t pow10_(int n) {
    int64_t r = 1;
    while (n-- > 0) r *= 10;
    return r;
}

static int parse_num(const char* s, num_t* out) {
    const char* p = s;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    int64_t v = 0;
    int digits = 0, prec = -1, whole = 0;
    for (; *p; p++) {
        if (*p == '.' && prec < 0) {
            prec = 0;
        } else if (*p >= '0' && *p <= '9') {
            if (digits >= 17 || (prec >= 0 && prec >= 9)) {
                dprintf(STDERR_FILENO, "seq: number too long or too precise: '%s'\n", s);
                return -1;
            }
            v = v * 10 + (*p - '0');
            digits++;
            if (prec >= 0) prec++;
            else whole++;
        } else {
            dprintf(STDERR_FILENO, "seq: invalid floating point argument: '%s'\n", s);
            return -1;
        }
    }
    if (!digits) {
        dprintf(STDERR_FILENO, "seq: invalid floating point argument: '%s'\n", s);
        return -1;
    }
    out->v = neg ? -v : v;
    out->prec = prec < 0 ? 0 : prec;
    out->width = (neg ? 1 : 0) + (whole ? whole : 1);
    return 0;
}

// v (times 10^prec) as text in buf. Returns the length.
static int fmt(char* buf, int64_t v, int prec) {
    char tmp[40];
    int n = 0;
    int neg = v < 0;
    uint64_t a = neg ? (uint64_t)-v : (uint64_t)v;
    int places = 0;
    do {
        if (prec && places == prec) tmp[n++] = '.';
        tmp[n++] = (char)('0' + (int)(a % 10));
        a /= 10;
        places++;
    } while (a || places <= prec);
    int len = 0;
    if (neg) buf[len++] = '-';
    while (n) buf[len++] = tmp[--n];
    buf[len] = 0;
    return len;
}

int main(int argc, char** argv) {
    const char* sep = "\n";
    int width = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1] && !(argv[a][1] >= '0' && argv[a][1] <= '9') && argv[a][1] != '.'; a++) {
        const char* o = argv[a] + 1;
        if (!strcmp(argv[a], "--")) {
            a++;
            break;
        }
        for (; *o; o++) {
            if (*o == 'w') width = 1;
            else if (*o == 's') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                sep = v;
                break;
            } else {
                dprintf(STDERR_FILENO, "seq: invalid option -- '%c'\n", *o);
                return usage();
            }
        }
    }
    int n = argc - a;
    if (n < 1) {
        dprintf(STDERR_FILENO, "seq: missing operand\n");
        return usage();
    }
    if (n > 3) {
        dprintf(STDERR_FILENO, "seq: extra operand '%s'\n", argv[a + 3]);
        return usage();
    }
    num_t first = { 1, 0, 1 }, step = { 1, 0, 1 }, last;
    if (n >= 2 && parse_num(argv[a], &first)) return 1;
    if (n == 3 && parse_num(argv[a + 1], &step)) return 1;
    if (parse_num(argv[a + n - 1], &last)) return 1;
    if (step.v == 0) {
        dprintf(STDERR_FILENO, "seq: invalid zero increment value: '%s'\n", argv[a + 1]);
        return 1;
    }
    // everything at the precision of the most precise of FIRST and INCR
    int prec = first.prec > step.prec ? first.prec : step.prec;
    int lprec = last.prec > prec ? last.prec : prec;
    int64_t f = first.v * pow10_(lprec - first.prec);
    int64_t s = step.v * pow10_(lprec - step.prec);
    int64_t l = last.v * pow10_(lprec - last.prec);
    // The output has `prec` places; LAST's extra places only bound the count.
    int drop = lprec - prec;  // places to drop when printing (they are zeros for f and s)

    // -w pads to the width of FIRST or LAST as written, the larger, plus
    // the decimal places of the output
    char b[48];
    int w = first.width > last.width ? first.width : last.width;
    if (prec) w += prec + 1;

    lio_out_t out;
    lio_out_init(&out, STDOUT_FILENO, "seq");
    int firstout = 1;
    for (int64_t v = f; s > 0 ? v <= l : v >= l; v += s) {
        int len = fmt(b, v / pow10_(drop), prec);
        if (!firstout) lio_puts(&out, sep);
        firstout = 0;
        if (width) {  // zeros go after the sign
            int i = 0;
            if (b[0] == '-') {
                lio_putc(&out, '-');
                i = 1;
            }
            for (int pad = len; pad < w; pad++) lio_putc(&out, '0');
            lio_put(&out, b + i, (uint32_t)(len - i));
        } else {
            lio_put(&out, b, (uint32_t)len);
        }
    }
    if (!firstout) lio_putc(&out, '\n');
    lio_flush(&out);
    return 0;
}

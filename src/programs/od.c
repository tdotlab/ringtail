// od [-A d|o|x|n] [-j SKIP] [-N LENGTH] [-t TYPE]... [-v] [-w[N]]
//    [-a] [-b] [-c] [-d] [-h] [-i] [-l] [-o] [-s] [-x] [FILE...]
//
// Dumps the bytes of the FILEs, taken as one stream, or of standard
// input when none is named (or for "-"), as GNU od does. Each line
// holds 16 bytes (-w changes that) written in every format asked for,
// one format to a row:
//   0000000 062550 066154 005157
//   0000006
// The address in front is in octal by default (-A d, x, or n for none).
// The formats are -t TYPE, any number of them, or the old letters:
//   a  named characters (nul, soh ... sp ... del)      -a  is -t a
//   c  characters and escapes (\n, 101 for the rest)   -c  is -t c
//   d  signed decimal   u  unsigned decimal   o  octal   x  hex
// A d, u, o or x takes a size: 1 2 4 8 or C S I L (char, short, int,
// long), 4 when none is given; the old letters -b -d -h -i -l -o -s -x
// are o1 u2 x2 d4 d8 o2 d2 x2. A "z" after a type adds the printable
// characters of the line between > and <.
// A line equal to the one before it is replaced by a single "*" unless
// -v is given. -j drops that many bytes first and -N stops after that
// many; both take 0x hex, a leading 0 for octal and the suffixes b, k,
// m, G, T ... (and KB, MB ... for powers of 1000). The addresses shown count
// from the start of the stream. The last line is not padded, and a
// final line with the address reached closes the listing.
// Not done: floating-point types (-t f), the old "od FILE +OFFSET" form.
#include "ulib.h"
#include "bstream.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO,
        "usage: od [-A d|o|x|n] [-j SKIP] [-N LENGTH] [-t TYPE]... [-v] [-w[N]]\n"
        "          [-abcdhilosx] [FILE...]\n");
    return 2;
}

static lio_out_t out;

static void flush_out(void) { lio_flush(&out); }

static void fail(const char* fmt, const char* a, const char* b) {
    lio_flush(&out);
    dprintf(STDERR_FILENO, "od: ");
    dprintf(STDERR_FILENO, fmt, a, b);
    dprintf(STDERR_FILENO, "\n");
    sys_exit(1);
}

// ---- the formats ----

#define MAXSPEC 24
#define MAXWIDTH 65536

typedef struct {
    char kind;       // a c d o u x
    int size;        // bytes in one item: 1 2 4 8
    int z;           // the >text< column
    int fw;          // width of an item
    uint32_t per;    // items in a full line
    uint32_t extra;  // columns the line is short of the widest format, spread over the items
} spec_t;

static spec_t specs[MAXSPEC];
static int nspecs;

static int natural_width(char kind, int size) {
    switch (kind) {
    case 'o': return size == 1 ? 3 : size == 2 ? 6 : size == 4 ? 11 : 22;
    case 'x': return size * 2;
    case 'u': return size == 1 ? 3 : size == 2 ? 5 : size == 4 ? 10 : 20;
    case 'd': return size == 1 ? 4 : size == 2 ? 6 : size == 4 ? 11 : 20;
    default: return 3;
    }
}

static void add_spec(char kind, int size, int z) {
    if (nspecs == MAXSPEC) fail("too many output formats", 0, 0);
    spec_t* s = &specs[nspecs++];
    s->kind = kind;
    s->size = size;
    s->z = z;
    s->fw = natural_width(kind, size);
    s->per = s->extra = 0;
}

// Reads one -t argument: types one after another, as "x1x2z".
static void add_types(const char* arg) {
    const char* s = arg;
    while (*s) {
        char kind = *s++;
        int size = 1;
        if (kind == 'a' || kind == 'c') {
            // no size
        } else if (kind == 'd' || kind == 'o' || kind == 'u' || kind == 'x') {
            size = 4;
            if (*s == 'C') { size = 1; s++; }
            else if (*s == 'S') { size = 2; s++; }
            else if (*s == 'I') { size = 4; s++; }
            else if (*s == 'L') { size = 8; s++; }
            else if (*s >= '0' && *s <= '9') {
                uint32_t v = 0;
                for (; *s >= '0' && *s <= '9'; s++) if (v < 100000) v = v * 10 + (uint32_t)(*s - '0');
                if (v != 1 && v != 2 && v != 4 && v != 8) {
                    char n[16];
                    snprintf(n, sizeof n, "%u", v);
                    lio_flush(&out);
                    dprintf(STDERR_FILENO, "od: invalid type string '%s';\nthis system doesn't provide a %s-byte integral type\n", arg, n);
                    sys_exit(1);
                }
                size = (int)v;
            }
        } else if (kind == 'f') {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "od: floating-point types are not supported\n");
            sys_exit(2);
        } else {
            char c[2] = { kind, 0 };
            fail("invalid character '%s' in type string '%s'", c, arg);
        }
        int z = 0;
        if (*s == 'z') { z = 1; s++; }
        add_spec(kind, size, z);
    }
}

// ---- numbers in the arguments ----

// Reads a -j or -N value as C does (0x hex, leading 0 octal) followed by
// an optional suffix. 0 on success, 1 for no number, 2 for a bad suffix,
// 3 for more than fits in 64 bits. A value past 32 bits is kept as the
// largest 32-bit one: no file here is longer than that.
static int parse_amount(const char* s, uint32_t* out_v, int plain) {
    const uint64_t MAX = 0xFFFFFFFFFFFFFFFFull;
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    if (*s == '+') s++;
    unsigned base = 10;
    if (!plain && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        ((s[2] >= '0' && s[2] <= '9') || ((s[2] | 0x20) >= 'a' && (s[2] | 0x20) <= 'f'))) { base = 16; s += 2; }
    else if (!plain && s[0] == '0') base = 8;
    uint64_t v = 0;
    int digits = 0, over = 0;
    for (;; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (base == 16 && (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f') d = (unsigned)((*s | 0x20) - 'a' + 10);
        else break;
        if (d >= base) break;
        digits++;
        if (v > (MAX - d) / base) over = 1;
        else v = v * base + d;
    }
    if (!digits) return 1;
    if (*s && plain) return 2;
    if (*s) {
        // b is 512; k, m, G, T, P, E, Z, Y are powers of 1024, or of
        // 1000 when followed by B (and of 1024 again with iB).
        unsigned steps = 1;
        uint64_t unit = 1024;
        const char* t = s + 1;
        switch (*s) {
        case 'b': unit = 512; break;
        case 'k': case 'K': break;
        case 'm': case 'M': steps = 2; break;
        case 'G': steps = 3; break;
        case 'T': steps = 4; break;
        case 'P': steps = 5; break;
        case 'E': steps = 6; break;
        case 'Z': case 'Y': steps = 7; break;  // too big for any number but 0
        default: return 2;
        }
        if (*s != 'b') {
            if (t[0] == 'B') { unit = 1000; t++; }
            else if (t[0] == 'i' && t[1] == 'B') t += 2;
        }
        if (*t) return 2;
        for (unsigned i = 0; i < steps; i++) {
            if (v > MAX / unit) over = 1;
            else v *= unit;
        }
    }
    if (over) return 3;
    *out_v = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
    return 0;
}

// The value of option `label`, or exit with GNU's complaint about it.
static uint32_t amount(const char* label, const char* v, int plain) {
    uint32_t n = 0;
    switch (parse_amount(v, &n, plain)) {
    case 1: fail("invalid %s argument '%s'", label, v); break;
    case 2: fail("invalid suffix in %s argument '%s'", label, v); break;
    case 3: fail("%s argument '%s' too large", label, v); break;
    }
    return n;
}

// ---- the output ----

static const char* const aname[33] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "nl", "vt", "ff", "cr", "so", "si",
    "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "esc", "fs", "gs", "rs", "us", "sp",
};

static void spaces(uint32_t n) {
    static const char sp[32] = "                                ";
    while (n) {
        uint32_t k = n < 32 ? n : 32;
        lio_put(&out, sp, k);
        n -= k;
    }
}

// v in the base, with a minus sign in front if asked, written into the
// end of tmp; returns where it starts.
static char* digits(char* end, uint64_t v, unsigned base, int neg) {
    char* p = end;
    do { *--p = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) *--p = '-';
    return p;
}

static void put_item(const spec_t* s, const unsigned char* b, int avail, uint32_t pad) {
    uint64_t v = 0;
    for (int i = 0; i < s->size; i++) v |= (uint64_t)(i < avail ? b[i] : 0) << (8 * i);

    char tmp[32], one[2];
    char* end = tmp + sizeof tmp;
    const char* p;
    uint32_t len;
    char fill = ' ';
    switch (s->kind) {
    case 'a': {
        unsigned c = (unsigned)(v & 0x7F);
        if (c <= 32) p = aname[c];
        else if (c == 127) p = "del";
        else { one[0] = (char)c; one[1] = 0; p = one; }
        len = (uint32_t)strlen(p);
        break;
    }
    case 'c': {
        unsigned c = (unsigned)v;
        const char* e = c == 0 ? "\\0" : c == 7 ? "\\a" : c == 8 ? "\\b" : c == 9 ? "\\t" : c == 10 ? "\\n" :
                        c == 11 ? "\\v" : c == 12 ? "\\f" : c == 13 ? "\\r" : 0;
        if (e) p = e;
        else if (c >= 32 && c < 127) { one[0] = (char)c; one[1] = 0; p = one; }
        else {
            tmp[0] = (char)('0' + (c >> 6));
            tmp[1] = (char)('0' + ((c >> 3) & 7));
            tmp[2] = (char)('0' + (c & 7));
            tmp[3] = 0;
            p = tmp;
        }
        len = (uint32_t)strlen(p);
        break;
    }
    case 'o': case 'x': {
        char* q = digits(end, v, s->kind == 'o' ? 8 : 16, 0);
        p = q;
        len = (uint32_t)(end - q);
        fill = '0';
        break;
    }
    case 'u': {
        char* q = digits(end, v, 10, 0);
        p = q;
        len = (uint32_t)(end - q);
        break;
    }
    default: {  // d
        int64_t sv = s->size == 1 ? (int8_t)v : s->size == 2 ? (int16_t)v : s->size == 4 ? (int32_t)v : (int64_t)v;
        char* q = digits(end, sv < 0 ? 0 - (uint64_t)sv : (uint64_t)sv, 10, sv < 0);
        p = q;
        len = (uint32_t)(end - q);
        break;
    }
    }
    spaces(1 + pad);
    if (fill == ' ') spaces(len < (uint32_t)s->fw ? (uint32_t)s->fw - len : 0);
    else for (uint32_t i = len; i < (uint32_t)s->fw; i++) lio_putc(&out, '0');
    lio_put(&out, p, len);
}

static char radix = 'o';

static void set_radix(const char* v) {
    radix = v[0] ? v[0] : ' ';
    if (!strchr("doxn", radix))
        fail("invalid output address radix '%s'; it must be one character from [doxn]", (char[]){ radix, 0 }, 0);
}
static uint32_t addr_width;

static void put_addr(uint32_t a) {
    if (radix == 'n') return;
    char tmp[16];
    char* end = tmp + sizeof tmp;
    char* p = digits(end, a, radix == 'x' ? 16 : radix == 'd' ? 10 : 8, 0);
    for (int i = (int)(end - p); i < (int)addr_width; i++) lio_putc(&out, '0');
    lio_put(&out, p, (uint32_t)(end - p));
}

static uint32_t bpb;  // bytes in a line

// The formats are made to line up by spreading the columns a format is
// short of the widest one over its items: the first k items get
// ceil(k * extra / per) of them.
static uint32_t pad_upto(const spec_t* s, uint32_t k) {
    return (k * s->extra + s->per - 1) / s->per;
}

static uint32_t pad_before(const spec_t* s, uint32_t k) {
    return pad_upto(s, k + 1) - pad_upto(s, k);
}

static void put_row(uint32_t off, const unsigned char* b, uint32_t n) {
    for (int i = 0; i < nspecs; i++) {
        const spec_t* s = &specs[i];
        if (i == 0) put_addr(off);
        else spaces(addr_width);
        uint32_t items = (n + (uint32_t)s->size - 1) / (uint32_t)s->size;
        for (uint32_t k = 0; k < items; k++) {
            uint32_t at = k * (uint32_t)s->size;
            put_item(s, b + at, (int)(n - at < (uint32_t)s->size ? n - at : (uint32_t)s->size), pad_before(s, k));
        }
        if (s->z) {
            // Fill the line out to its full width, then the characters.
            uint32_t blank = (s->per - items) * (uint32_t)(s->fw + 1) + s->extra - pad_upto(s, items);
            spaces(blank + 2);
            lio_putc(&out, '>');
            for (uint32_t k = 0; k < n; k++) lio_putc(&out, b[k] >= 0x20 && b[k] < 0x7F ? (char)b[k] : '.');
            lio_putc(&out, '<');
        }
        lio_putc(&out, '\n');
    }
}

// ---- the options ----

// Which of the long options `arg` names, or -1. A name may be cut short:
// none of them starts like another, so any piece is enough.
static int long_opt(const char* arg, const char* const* names, int n) {
    const char* eq = strchr(arg, '=');
    uint32_t len = eq ? (uint32_t)(eq - arg) : (uint32_t)strlen(arg);
    len -= 2;
    for (int i = 0; i < n; i++)
        if (len && len <= strlen(names[i]) && !strncmp(arg + 2, names[i], len)) return i;
    return -1;
}

int main(int argc, char** argv) {
    lio_out_init(&out, STDOUT_FILENO, "od");
    int verbose = 0, a = 1, have_width = 0;
    uint32_t start = 0, limit = 0xFFFFFFFFu, width = 0;
    static const char* const lnames[] = { "address-radix", "skip-bytes", "read-bytes", "format", "output-duplicates", "width" };

    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char* arg = argv[a];
        if (!strcmp(arg, "--")) { a++; break; }
        if (arg[1] == '-') {
            int which = long_opt(arg, lnames, 6);
            if (which < 0) return usage();
            const char* eq = strchr(arg, '=');
            const char* v = eq ? eq + 1 : 0;
            if (which == 4) {
                if (v) return usage();
                verbose = 1;
                continue;
            }
            if (which == 5) {  // --width takes its value only after "="
                have_width = 1;
                width = v ? amount("--width", v, 1) : 32;
                if (v && !width) fail("invalid --width argument '%s'", v, 0);
                continue;
            }
            if (!v) {
                if (++a == argc) return usage();
                v = argv[a];
            }
            if (which == 0) set_radix(v);
            else if (which == 1) start = amount("--skip-bytes", v, 0);
            else if (which == 2) limit = amount("--read-bytes", v, 0);
            else add_types(v);
            continue;
        }
        for (const char* o = arg + 1; *o; o++) {
            switch (*o) {
            case 'a': add_spec('a', 1, 0); break;
            case 'b': add_spec('o', 1, 0); break;
            case 'c': add_spec('c', 1, 0); break;
            case 'd': add_spec('u', 2, 0); break;
            case 'h': case 'x': add_spec('x', 2, 0); break;
            case 'i': add_spec('d', 4, 0); break;
            case 'l': add_spec('d', 8, 0); break;
            case 'o': add_spec('o', 2, 0); break;
            case 's': add_spec('d', 2, 0); break;
            case 'v': verbose = 1; break;
            case 'w':
                // An optional value, written right after the letter.
                have_width = 1;
                width = o[1] ? amount("-w", o + 1, 1) : 32;
                if (o[1] && !width) fail("invalid -w argument '%s'", o + 1, 0);
                o += strlen(o) - 1;
                break;
            case 'A': case 'j': case 'N': case 't': {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (*o == 'A') set_radix(v);
                else if (*o == 'j') start = amount("-j", v, 0);
                else if (*o == 'N') limit = amount("-N", v, 0);
                else add_types(v);
                o += strlen(o) - 1;
                break;
            }
            default:
                return usage();
            }
        }
    }
    if (a == argc && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();
    if (!nspecs) add_spec('o', 2, 0);
    addr_width = radix == 'n' ? 0 : radix == 'x' ? 6 : 7;

    // The line: as wide as the biggest item, 16 bytes unless -w says
    // otherwise, and the rows for the different formats made to line up.
    uint32_t big = 1;
    for (int i = 0; i < nspecs; i++) if ((uint32_t)specs[i].size > big) big = (uint32_t)specs[i].size;
    bpb = 16;
    uint32_t bad_width = 0;
    if (have_width) {
        if (width % big) {
            bad_width = width;
            width = big;
        }
        if (width > MAXWIDTH) fail("-w argument too large (at most 65536)", 0, 0);
        bpb = width;
    }
    uint32_t wide = 0;
    for (int i = 0; i < nspecs; i++) {
        uint32_t w = (uint32_t)(specs[i].fw + 1) * (bpb / (uint32_t)specs[i].size);
        if (w > wide) wide = w;
    }
    for (int i = 0; i < nspecs; i++) {
        specs[i].per = bpb / (uint32_t)specs[i].size;
        specs[i].extra = wide - (uint32_t)(specs[i].fw + 1) * specs[i].per;
    }

    unsigned char* cur = malloc(bpb);
    unsigned char* prev = malloc(bpb);
    if (!cur || !prev) fail("out of memory", 0, 0);

    bstream_t in;
    bs_init(&in, "od", argv + a, argc - a, start, flush_out);
    if (!bs_start(&in)) fail("cannot skip past end of combined input", 0, 0);
    if (bad_width) {
        char n1[16], n2[16];
        snprintf(n1, sizeof n1, "%u", bad_width);
        snprintf(n2, sizeof n2, "%u", bpb);
        lio_flush(&out);
        dprintf(STDERR_FILENO, "od: warning: invalid width %s; using %s instead\n", n1, n2);
    }

    uint32_t off = start;
    int have_prev = 0, starred = 0;
    while (limit) {
        uint32_t n = bs_get(&in, cur, limit < bpb ? limit : bpb);
        if (!n) break;
        limit -= n;
        if (n == bpb && have_prev && !verbose && !memcmp(cur, prev, bpb)) {
            if (!starred) lio_puts(&out, "*\n");
            starred = 1;
            off += n;
            continue;
        }
        starred = 0;
        put_row(off, cur, n);
        have_prev = 1;
        memcpy(prev, cur, bpb);
        off += n;
    }
    if (in.opened && radix != 'n') {
        put_addr(off);
        lio_putc(&out, '\n');
    }
    lio_flush(&out);
    return in.status;
}

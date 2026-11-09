// tr [-c | -C] [-d] [-s] [-t] SET1 [SET2]
//
// Translates, squeezes and deletes the bytes of standard input.
//   tr SET1 SET2   each byte of SET1 becomes the byte at the same place
//                  in SET2 (a short SET2 repeats its last byte; -t cuts
//                  SET1 down to SET2's length instead)
//   tr -d SET1     deletes the bytes of SET1
//   tr -s SET1     squeezes each run of a SET1 byte into one
//   tr -s SET1 SET2    translates, then squeezes the bytes of SET2
//   tr -ds SET1 SET2   deletes SET1, then squeezes the bytes of SET2
//   -c, -C  use every byte NOT in SET1, in ascending order
// A set is made of plain bytes, a-z ranges, [:alpha:] [:digit:] [:alnum:]
// [:upper:] [:lower:] [:space:] [:blank:] [:punct:] [:print:] [:graph:]
// [:cntrl:] [:xdigit:] classes, [=c=] (the byte c), and the escapes \\ \a
// \b \f \n \r \t \v \NNN (octal). In SET2, [c*N] is N copies of c ([c*]
// pads SET2 out to the length of SET1).
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: tr [-cCdst] SET1 [SET2]\n");
    return 2;
}

#define SET_MAX 8192

typedef struct {
    uint16_t* v;
    uint32_t n;
    int fill;          // [c*] was given: this byte pads the set to the length of SET1
    uint32_t fill_at;  // ... at this place
} set_t;

static int fail(const char* what, const char* arg) {
    if (arg) dprintf(STDERR_FILENO, "tr: %s '%s'\n", what, arg);
    else dprintf(STDERR_FILENO, "tr: %s\n", what);
    return -1;
}

static int add(set_t* s, unsigned c) {
    if (s->n == SET_MAX) return fail("set too long", 0);
    s->v[s->n++] = (uint16_t)c;
    return 0;
}

static int in_class(const char* name, uint32_t len, int c) {
#define IS(lit) (len == sizeof lit - 1 && !memcmp(name, lit, len))
    if (IS("alpha")) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (IS("digit")) return c >= '0' && c <= '9';
    if (IS("alnum")) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (IS("upper")) return c >= 'A' && c <= 'Z';
    if (IS("lower")) return c >= 'a' && c <= 'z';
    if (IS("space")) return c == ' ' || (c >= 9 && c <= 13);
    if (IS("blank")) return c == ' ' || c == '\t';
    if (IS("punct")) return c > 32 && c < 127 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    if (IS("print")) return c >= 32 && c < 127;
    if (IS("graph")) return c > 32 && c < 127;
    if (IS("cntrl")) return c < 32 || c == 127;
    if (IS("xdigit")) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
#undef IS
    return -1;
}

// One element at *pp: a byte (with its escape taken apart), or -1 at the end.
static int next_byte(const char** pp, int* escaped) {
    const char* p = *pp;
    *escaped = 0;
    if (!*p) return -1;
    if (*p != '\\') {
        *pp = p + 1;
        return (unsigned char)*p;
    }
    *escaped = 1;
    p++;
    int c;
    switch (*p) {
    case 0: *pp = p; return '\\';  // a lone backslash at the end is itself
    case 'a': c = 7; break;
    case 'b': c = 8; break;
    case 'f': c = 12; break;
    case 'n': c = 10; break;
    case 'r': c = 13; break;
    case 't': c = 9; break;
    case 'v': c = 11; break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = 0, k = 0;
        while (k < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p++ - '0');
            k++;
        }
        *pp = p;
        return v & 255;
    }
    default: c = (unsigned char)*p; break;
    }
    *pp = p + 1;
    return c;
}

// Expands the text of a set. is2: it is SET2 (where [c*N] is allowed).
static int expand(const char* text, set_t* s, int is2) {
    s->v = malloc(SET_MAX * sizeof *s->v);
    if (!s->v) return fail("out of memory", 0);
    s->n = 0;
    s->fill = -1;
    const char* p = text;
    while (*p) {
        if (p[0] == '[' && p[1] == ':') {
            const char* e = p + 2;
            while (*e && !(e[0] == ':' && e[1] == ']')) e++;
            if (*e) {
                uint32_t len = (uint32_t)(e - (p + 2));
                if (in_class(p + 2, len, 'a') < 0) {
                    char nm[24];
                    snprintf(nm, sizeof nm, "%.*s", (int)(len < 20 ? len : 20), p + 2);
                    return fail("invalid character class", nm);
                }
                for (int c = 0; c < 256; c++)
                    if (in_class(p + 2, len, c) > 0 && add(s, (unsigned)c)) return -1;
                p = e + 2;
                continue;
            }
        }
        if (p[0] == '[' && p[1] == '=' && p[2] && p[3] == '=' && p[4] == ']') {
            if (add(s, (unsigned char)p[2])) return -1;
            p += 5;
            continue;
        }
        if (p[0] == '[' && p[1]) {
            // [c*N] and [c*]
            const char* q = p + 1;
            int esc;
            int c = next_byte(&q, &esc);
            if (c >= 0 && *q == '*') {
                const char* r = q + 1;
                uint32_t n = 0;
                int digits = 0, base = *r == '0' ? 8 : 10;
                while (*r >= '0' && *r <= (base == 8 ? '7' : '9')) {
                    n = n * (uint32_t)base + (uint32_t)(*r++ - '0');
                    digits = 1;
                    if (n > SET_MAX) return fail("repeat count too large", 0);
                }
                if (*r == ']') {
                    if (!is2) return fail("the [c*] construct may appear in string2 only", 0);
                    if (!digits) {
                        if (s->fill >= 0) return fail("only one [c*] repeat construct may appear in string2", 0);
                        s->fill = c;
                        s->fill_at = s->n;
                    } else {
                        for (uint32_t k = 0; k < n; k++)
                            if (add(s, (unsigned)c)) return -1;
                    }
                    p = r + 1;
                    continue;
                }
            }
        }
        int esc, lo = next_byte(&p, &esc);
        if (*p == '-' && p[1]) {
            const char* q = p + 1;
            int esc2, hi = next_byte(&q, &esc2);
            if (hi < lo) {
                char rng[16];
                snprintf(rng, sizeof rng, "%c-%c", lo, hi);
                return fail("range-endpoints are in reverse collating sequence order:", rng);
            }
            for (int c = lo; c <= hi; c++)
                if (add(s, (unsigned)c)) return -1;
            p = q;
        } else if (add(s, (unsigned)lo)) {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    int opt_c = 0, opt_d = 0, opt_s = 0, opt_t = 0;
    const char* ops[3];
    int nops = 0, only_ops = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            if (nops == 3) {
                dprintf(STDERR_FILENO, "tr: extra operand '%s'\n", s);
                return usage();
            }
            ops[nops++] = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (!strcmp(s, "--complement")) opt_c = 1;
        else if (!strcmp(s, "--delete")) opt_d = 1;
        else if (!strcmp(s, "--squeeze-repeats")) opt_s = 1;
        else if (!strcmp(s, "--truncate-set1")) opt_t = 1;
        else if (s[1] == '-') {
            dprintf(STDERR_FILENO, "tr: unrecognized option '%s'\n", s);
            return usage();
        } else {
            for (const char* o = s + 1; *o; o++) {
                if (*o == 'c' || *o == 'C') opt_c = 1;
                else if (*o == 'd') opt_d = 1;
                else if (*o == 's') opt_s = 1;
                else if (*o == 't') opt_t = 1;
                else {
                    dprintf(STDERR_FILENO, "tr: invalid option -- '%c'\n", *o);
                    return usage();
                }
            }
        }
    }
    if (nops == 0) {
        dprintf(STDERR_FILENO, "tr: missing operand\n");
        return usage();
    }
    int translating = !opt_d && nops == 2;
    if (opt_d && !opt_s && nops > 1) {
        dprintf(STDERR_FILENO, "tr: extra operand '%s'\n", ops[1]);
        return usage();
    }
    if (nops > 2) {
        dprintf(STDERR_FILENO, "tr: extra operand '%s'\n", ops[2]);
        return usage();
    }
    if (!opt_d && !opt_s && nops == 1) {
        dprintf(STDERR_FILENO, "tr: missing operand after '%s'\n", ops[0]);
        dprintf(STDERR_FILENO, "Two strings must be given when translating.\n");
        return usage();
    }
    if (opt_d && opt_s && nops == 1) {
        dprintf(STDERR_FILENO, "tr: missing operand after '%s'\n", ops[0]);
        dprintf(STDERR_FILENO, "Two strings must be given when both deleting and squeezing repeats.\n");
        return usage();
    }
    if (opt_s && !opt_d && nops == 1) translating = 0;

    set_t s1 = { 0, 0, -1, 0 }, s2 = { 0, 0, -1, 0 };
    if (expand(ops[0], &s1, 0)) return 2;
    uint8_t in1[256] = { 0 };
    for (uint32_t i = 0; i < s1.n; i++) in1[s1.v[i]] = 1;
    if (opt_c) {
        s1.n = 0;
        for (int c = 0; c < 256; c++) {
            in1[c] = !in1[c];
            if (in1[c]) s1.v[s1.n++] = (uint16_t)c;
        }
    }
    if (nops == 2 && expand(ops[1], &s2, 1)) return 2;
    if (s2.fill >= 0) {
        // [c*] pads SET2 to the length of SET1
        uint32_t want = s1.n > s2.n ? s1.n - s2.n : 0;
        if (s2.n + want > SET_MAX) want = SET_MAX - s2.n;
        for (uint32_t i = s2.n; i-- > s2.fill_at;) s2.v[i + want] = s2.v[i];
        for (uint32_t i = 0; i < want; i++) s2.v[s2.fill_at + i] = (uint16_t)s2.fill;
        s2.n += want;
    }

    int map[256];
    for (int c = 0; c < 256; c++) map[c] = c;
    uint8_t squeeze[256] = { 0 };
    if (translating) {
        if (!s2.n && s1.n && !opt_t) {
            dprintf(STDERR_FILENO, "tr: when not truncating set1, string2 must be non-empty\n");
            return 2;
        }
        uint32_t n1 = s1.n;
        if (opt_t && s2.n < n1) n1 = s2.n;
        for (uint32_t i = 0; i < n1; i++) map[s1.v[i]] = s2.v[i < s2.n ? i : s2.n - 1];
        if (opt_s)
            for (uint32_t i = 0; i < s2.n; i++) squeeze[s2.v[i]] = 1;
    } else if (opt_d && opt_s) {
        for (uint32_t i = 0; i < s2.n; i++) squeeze[s2.v[i]] = 1;
    } else if (opt_s) {
        for (int c = 0; c < 256; c++) squeeze[c] = in1[c];
    }

    lio_out_t out;
    lio_out_init(&out, STDOUT_FILENO, "tr");
    lio_in_t in;
    lio_in_init(&in, STDIN_FILENO);
    int last = -1, c;
    while ((c = lio_getc(&in, &out)) >= 0) {
        if (opt_d && in1[c]) continue;
        int o = map[c];
        if (squeeze[o] && o == last) continue;
        last = o;
        lio_putc(&out, (char)o);
    }
    lio_flush(&out);
    if (in.err) {
        dprintf(STDERR_FILENO, "tr: read error: %s\n", strerror(in.err));
        return 1;
    }
    return 0;
}

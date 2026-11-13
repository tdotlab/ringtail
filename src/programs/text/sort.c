// sort [-bcfnrsu] [-k F1[,F2]] [-t SEP] [-o FILE] [FILE...]    sort lines
//
// Sorts the lines of every FILE together, or of standard input when none
// is named (or for "-"), and prints them. Plain order is bytewise.
//   -r  reverse          -f  ignore case        -n  compare as numbers
//   -b  ignore leading blanks in the key        -u  drop lines whose key
//   -s  keep input order for equal keys             equals the one before
//   -c  only check: say so and exit 1 at the first line out of order
//   -o FILE   write there instead (FILE may be one of the inputs)
//   -k F1[,F2]  sort on fields F1 to F2 (to the end of the line without
//       F2); fields are separated by blanks, the blanks leading a field
//       belonging to it, or by the single character given with -t.
//       One key only: the letters b, f, n and r after it, as in
//       -k2,2n, are the same as giving those flags.
// Options may come after the files; "--" ends them.
//
// Lines that compare equal on the key are then put in bytewise order of
// the whole line (the way GNU sort does) unless -s or -u is given, in
// which case they keep the order they came in, and -u keeps the first.
// A number is optional blanks, a minus sign, digits and a fraction, and
// anything else counts as zero; it is compared digit by digit, so no
// size of number overflows.
//
// Everything is read before anything is written: the input is held in
// one buffer plus 24 bytes of index per line. A last line with no
// newline gets one. A file that cannot be read stops sort with status 2
// and no output.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: sort [-bcfnrsu] [-k F1[,F2]] [-t SEP] [-o FILE] [FILE...]\n");
    return 2;
}

static int opt_b, opt_c, opt_f, opt_n, opt_r, opt_s, opt_u;
static int sep = -1;         // -t: the field separator; -1 means blanks
static uint32_t key1, key2;  // -k: first and last field, 1-based; 0 for none
static const char* outname;

typedef struct {
    const char* p;           // the line, without its newline
    uint32_t n;
    const char* k;           // the part of it to sort on
    uint32_t kn;
} line_t;

static char* data;           // every input, each line ending in '\n'
static uint32_t dlen, dcap;
static line_t* lines;
static line_t** ord;         // pointers to lines, in output order; 2n slots, the upper half scratch
static uint32_t nlines;

static int is_blank(char c) { return c == ' ' || c == '\t'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static unsigned char up(unsigned char c) { return c >= 'a' && c <= 'z' ? (unsigned char)(c - 32) : c; }

static void oom(void) {
    dprintf(STDERR_FILENO, "sort: out of memory\n");
    sys_exit(2);
}

static void reserve(uint32_t extra) {
    if (dlen + extra <= dcap) return;
    uint32_t want = dcap ? dcap : 65536;
    while (want < dlen + extra) {
        if (want > 0x20000000u) oom();
        want *= 2;
    }
    char* p = realloc(data, want);
    if (!p) oom();
    data = p;
    dcap = want;
}

// Appends the whole of fd to data. 0, or -1 after saying why not.
static int slurp(int fd, const char* name) {
    int n;
    for (;;) {
        reserve(4096);
        uint32_t room = dcap - dlen;
        n = sys_read(fd, data + dlen, room > 65536 ? 65536 : room);
        if (n <= 0) break;
        dlen += (uint32_t)n;
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "sort: %s: %s\n", name, strerror(n));
        return -1;
    }
    if (dlen && data[dlen - 1] != '\n') {
        reserve(1);
        data[dlen++] = '\n';
    }
    return 0;
}

// Where field f (1-based) of the n bytes at p starts and ends. A field
// that is not there is empty and at the end of the line.
static void field(const char* p, uint32_t n, uint32_t f, uint32_t* s, uint32_t* e) {
    uint32_t i = 0, st = 0;
    for (uint32_t k = 1; k <= f; k++) {
        st = i;
        if (sep >= 0) {
            while (i < n && (unsigned char)p[i] != (unsigned)sep) i++;
            if (k < f) {
                if (i == n) { st = n; break; }
                i++;  // past the separator
            }
        } else {
            while (i < n && is_blank(p[i])) i++;
            while (i < n && !is_blank(p[i])) i++;
        }
    }
    *s = st;
    *e = i;
}

static void make_key(line_t* l) {
    uint32_t s = 0, e = l->n, t;
    if (key1) {
        field(l->p, l->n, key1, &s, &t);
        if (key2) field(l->p, l->n, key2, &t, &e);
    }
    if (opt_b)
        while (s < e && is_blank(l->p[s])) s++;
    if (e < s) e = s;
    l->k = l->p + s;
    l->kn = e - s;
}

static int text_cmp(const char* a, uint32_t an, const char* b, uint32_t bn) {
    uint32_t m = an < bn ? an : bn;
    if (!opt_f) {
        int c = memcmp(a, b, m);
        if (c) return c < 0 ? -1 : 1;
    } else {
        for (uint32_t i = 0; i < m; i++) {
            unsigned char x = up((unsigned char)a[i]), y = up((unsigned char)b[i]);
            if (x != y) return x < y ? -1 : 1;
        }
    }
    return an < bn ? -1 : an > bn;
}

// A number split into its parts: no leading zeros in the integer part,
// no trailing zeros in the fraction, and negative zero is just zero.
typedef struct {
    int neg;
    const char* ip;
    uint32_t il;
    const char* fp;
    uint32_t fl;
} num_t;

static num_t num_parse(const char* p, uint32_t n) {
    num_t r = { 0, p, 0, p, 0 };
    uint32_t i = 0;
    while (i < n && is_blank(p[i])) i++;
    if (i < n && p[i] == '-') {
        r.neg = 1;
        i++;
    }
    uint32_t s = i;
    while (i < n && is_digit(p[i])) i++;
    while (s < i && p[s] == '0') s++;
    r.ip = p + s;
    r.il = i - s;
    if (i < n && p[i] == '.') {
        uint32_t fs = ++i;
        while (i < n && is_digit(p[i])) i++;
        uint32_t fe = i;
        while (fe > fs && p[fe - 1] == '0') fe--;
        r.fp = p + fs;
        r.fl = fe - fs;
    }
    if (!r.il && !r.fl) r.neg = 0;
    return r;
}

static int num_cmp(const char* a, uint32_t an, const char* b, uint32_t bn) {
    num_t x = num_parse(a, an), y = num_parse(b, bn);
    if (x.neg != y.neg) return x.neg ? -1 : 1;
    int c;
    if (x.il != y.il) {
        c = x.il < y.il ? -1 : 1;
    } else if ((c = memcmp(x.ip, y.ip, x.il)) != 0) {
        c = c < 0 ? -1 : 1;
    } else {
        uint32_t m = x.fl < y.fl ? x.fl : y.fl;
        if ((c = memcmp(x.fp, y.fp, m)) != 0) c = c < 0 ? -1 : 1;
        else c = x.fl < y.fl ? -1 : x.fl > y.fl;
    }
    return x.neg ? -c : c;
}

// The order lines are sorted into. 0 means equal for -u's purposes.
static int line_cmp(const line_t* a, const line_t* b) {
    int c = opt_n ? num_cmp(a->k, a->kn, b->k, b->kn) : text_cmp(a->k, a->kn, b->k, b->kn);
    if (opt_r) c = -c;
    if (c || opt_s || opt_u) return c;
    uint32_t m = a->n < b->n ? a->n : b->n;
    c = memcmp(a->p, b->p, m);
    if (!c) c = a->n < b->n ? -1 : a->n > b->n;
    else c = c < 0 ? -1 : 1;
    return opt_r ? -c : c;
}

// Stable: on a tie the left one goes first.
static void msort(line_t** a, line_t** t, uint32_t n) {
    if (n < 2) return;
    if (n <= 8) {
        for (uint32_t i = 1; i < n; i++) {
            line_t* x = a[i];
            uint32_t j = i;
            while (j && line_cmp(a[j - 1], x) > 0) {
                a[j] = a[j - 1];
                j--;
            }
            a[j] = x;
        }
        return;
    }
    uint32_t h = n / 2;
    msort(a, t, h);
    msort(a + h, t, n - h);
    if (line_cmp(a[h - 1], a[h]) <= 0) return;  // already in order
    memcpy(t, a, h * sizeof *a);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n) a[k++] = line_cmp(a[j], t[i]) < 0 ? a[j++] : t[i++];
    while (i < h) a[k++] = t[i++];
}

static char ob[4096];
static uint32_t on;
static int ofd = STDOUT_FILENO;

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(ofd, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "sort: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(2);
        }
        p += w;
        on -= (uint32_t)w;
    }
}

static void emit(const char* p, uint32_t n) {
    while (n) {
        uint32_t take = sizeof ob - on < n ? (uint32_t)sizeof ob - on : n;
        memcpy(ob + on, p, take);
        on += take;
        p += take;
        n -= take;
        if (on == sizeof ob) flush();
    }
}

// "2" or "2,3", with b, f, n or r after either.
static int parse_key(const char* v) {
    const char* s = v;
    uint32_t f[2] = { 0, 0 };
    for (int part = 0; part < 2; part++) {
        if (!is_digit(*s)) return -1;
        for (; is_digit(*s); s++) {
            if (f[part] > 100000000u) return -1;
            f[part] = f[part] * 10 + (uint32_t)(*s - '0');
        }
        if (!f[part]) return -1;
        for (; *s && *s != ','; s++) {
            if (*s == 'b') opt_b = 1;
            else if (*s == 'f') opt_f = 1;
            else if (*s == 'n') opt_n = 1;
            else if (*s == 'r') opt_r = 1;
            else return -1;
        }
        if (!*s) break;
        s++;  // the comma
        if (part == 1) return -1;
        if (!*s) return -1;
    }
    key1 = f[0];
    key2 = f[1];
    return 0;
}

int main(int argc, char** argv) {
    const char** files = malloc((size_t)argc * sizeof *files);
    if (!files) oom();
    int nf = 0, only_files = 0, keyed = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_files || s[0] != '-' || !s[1]) {
            files[nf++] = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_files = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            int took = 0;
            switch (*o) {
            case 'b': opt_b = 1; break;
            case 'c': opt_c = 1; break;
            case 'f': opt_f = 1; break;
            case 'n': opt_n = 1; break;
            case 'r': opt_r = 1; break;
            case 's': opt_s = 1; break;
            case 'u': opt_u = 1; break;
            case 'k':
            case 't':
            case 'o': {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                took = 1;
                if (*o == 'k') {
                    if (keyed++) {
                        dprintf(STDERR_FILENO, "sort: only one -k key is supported\n");
                        return 2;
                    }
                    if (parse_key(v)) {
                        dprintf(STDERR_FILENO, "sort: invalid key: '%s'\n", v);
                        return 2;
                    }
                } else if (*o == 't') {
                    if (strlen(v) != 1) {
                        dprintf(STDERR_FILENO, "sort: the separator must be one character: '%s'\n", v);
                        return 2;
                    }
                    sep = (unsigned char)v[0];
                } else {
                    outname = v;
                }
                break;
            }
            default:
                return usage();
            }
            if (took) break;
        }
    }

    if (opt_c && nf > 1) {
        dprintf(STDERR_FILENO, "sort: extra operand '%s' not allowed with -c\n", files[1]);
        return 2;
    }
    const char* first = nf ? files[0] : "-";
    for (int i = 0; i < nf || (i == 0 && nf == 0); i++) {
        const char* name = nf ? files[i] : "-";
        int std = !strcmp(name, "-");
        int fd = std ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sort: %s: %s\n", name, strerror(fd));
            return 2;
        }
        int bad = slurp(fd, std ? "standard input" : name);
        if (!std) sys_close(fd);
        if (bad) return 2;
    }

    for (uint32_t i = 0; i < dlen; i++)
        if (data[i] == '\n') nlines++;
    if (nlines > 16u * 1024 * 1024) oom();
    if (nlines) {
        lines = malloc(nlines * sizeof *lines);
        ord = malloc(nlines * 2 * sizeof *ord);
        if (!lines || !ord) oom();
        uint32_t start = 0, n = 0;
        for (uint32_t i = 0; i < dlen; i++) {
            if (data[i] != '\n') continue;
            lines[n].p = data + start;
            lines[n].n = i - start;
            make_key(&lines[n]);
            ord[n] = &lines[n];
            n++;
            start = i + 1;
        }
    }

    if (opt_c) {
        for (uint32_t i = 1; i < nlines; i++) {
            int c = line_cmp(ord[i - 1], ord[i]);
            if (opt_u ? c >= 0 : c > 0) {
                dprintf(STDERR_FILENO, "sort: %s:%u: disorder: ", first, i + 1);
                sys_write(STDERR_FILENO, ord[i]->p, ord[i]->n);
                sys_write(STDERR_FILENO, "\n", 1);
                return 1;
            }
        }
        return 0;
    }

    msort(ord, ord + nlines, nlines);

    if (outname) {
        ofd = sys_open(outname, O_WRONLY | O_CREAT | O_TRUNC);
        if (ofd < 0) {
            dprintf(STDERR_FILENO, "sort: %s: %s\n", outname, strerror(ofd));
            return 2;
        }
    }
    const line_t* last = 0;
    for (uint32_t i = 0; i < nlines; i++) {
        const line_t* l = ord[i];
        if (opt_u && last && line_cmp(last, l) == 0) continue;
        last = l;
        emit(l->p, l->n);
        emit("\n", 1);
    }
    flush();
    if (outname) sys_close(ofd);
    return 0;
}

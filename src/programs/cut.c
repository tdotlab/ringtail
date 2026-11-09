// cut -b LIST | -c LIST | -f LIST [-d DELIM] [-s] [--complement]
//     [--output-delimiter=STR] [FILE...]
//
// Prints chosen parts of each line of the FILEs (or standard input, or
// "-"). LIST is positions counted from 1, as N, N-M, N- (to the end) or
// -M (from the start), separated by commas.
//   -b, -c  select bytes (the console is 8-bit, so they are the same)
//   -f      select fields, which are separated by DELIM (a tab unless -d
//           says another single character); a line with no DELIM in it
//           is passed through whole, unless -s drops it
//   -d C    the field delimiter
//   -s      with -f, print only lines that contain the delimiter
//   -n      accepted and ignored
//   --complement           the parts NOT in LIST
//   --output-delimiter=S   S between the selected parts, instead of
//                          DELIM (fields) or nothing (bytes); an empty S
//                          is a NUL byte
// A last line with no newline is written with one.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: cut -b|-c|-f LIST [-d DELIM] [-s] [--complement] [--output-delimiter=STR] [FILE...]\n");
    return 2;
}

#define MAX_RANGES 64

typedef struct {
    uint32_t lo, hi;  // 1-based, inclusive; hi may be 0xFFFFFFFF for "to the end"
} range_t;

static range_t ranges[MAX_RANGES];
static int nranges;
static int mode;          // 'b' (also c) or 'f'
static int only_delim, complement;
static char delim = '\t';
static const char* out_delim;  // 0: the default for the mode
static uint32_t out_delim_len;

static lio_out_t out;

// Digits at *pp into *out, moving past them. 1 if there were any, 0 if not,
// -1 after a message for one too big.
static int digits(const char** pp, uint32_t* out) {
    const char* p = *pp;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p++ - '0');
        if (v > 0xFFFFFFFEu) {
            dprintf(STDERR_FILENO, "cut: invalid range: position too large\n");
            return -1;
        }
    }
    if (p == *pp) return 0;
    *pp = p;
    *out = (uint32_t)v;
    return 1;
}

// "N", "N-M", "N-" or "-M" from *s; advances past it. 0, or -1 after a message.
static int parse_range(const char** s, range_t* r) {
    const char* p = *s;
    uint32_t lo = 0, hi = 0;
    int have_lo = digits(&p, &lo), have_hi = 0;
    if (have_lo < 0) return -1;
    if (*p == '-') {
        p++;
        have_hi = digits(&p, &hi);
        if (have_hi < 0) return -1;
        if (!have_lo && !have_hi) {
            dprintf(STDERR_FILENO, "cut: invalid range with no endpoint: -\n");
            return -1;
        }
        if (!have_lo) lo = 1;
        if (!have_hi) hi = 0xFFFFFFFFu;
    } else if (have_lo) {
        hi = lo;
    } else {
        dprintf(STDERR_FILENO, "cut: invalid byte, character or field list\n");
        return -1;
    }
    if (*p && *p != ',' && *p != ' ' && *p != '\t') {
        dprintf(STDERR_FILENO, "cut: invalid byte, character or field list\n");
        return -1;
    }
    if (lo == 0) {
        dprintf(STDERR_FILENO, "cut: %s are numbered from 1\n", mode == 'f' ? "fields" : "byte/character positions");
        return -1;
    }
    if (hi < lo) {
        dprintf(STDERR_FILENO, "cut: invalid decreasing range\n");
        return -1;
    }
    r->lo = lo;
    r->hi = hi;
    *s = p;
    return 0;
}

static int by_lo(const range_t* a, const range_t* b) { return a->lo < b->lo ? -1 : a->lo > b->lo; }

// Parses LIST into ranges: sorted, with overlapping ranges merged (touching ones stay apart, so
// --output-delimiter shows the seam, as GNU does).
static int parse_list(const char* s) {
    for (;;) {
        if (nranges == MAX_RANGES) {
            dprintf(STDERR_FILENO, "cut: too many ranges in the list\n");
            return -1;
        }
        if (parse_range(&s, &ranges[nranges])) return -1;
        nranges++;
        if (!*s) break;
        s++;  // the separator
        if (!*s) {
            dprintf(STDERR_FILENO, "cut: invalid byte, character or field list\n");
            return -1;
        }
    }
    // insertion sort by start (the list is short), then merge
    for (int i = 1; i < nranges; i++) {
        range_t t = ranges[i];
        int j = i;
        while (j > 0 && by_lo(&ranges[j - 1], &t) > 0) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j] = t;
    }
    int m = 0;
    for (int i = 1; i < nranges; i++) {
        if (ranges[i].lo <= ranges[m].hi) {
            if (ranges[i].hi > ranges[m].hi) ranges[m].hi = ranges[i].hi;
        } else {
            ranges[++m] = ranges[i];
        }
    }
    nranges = m + 1;
    if (complement) {
        range_t inv[MAX_RANGES + 1];
        int k = 0;
        uint32_t next = 1;
        int open = 1;  // is there anything left after the last range?
        for (int i = 0; i < nranges; i++) {
            if (ranges[i].lo > next) {
                inv[k].lo = next;
                inv[k].hi = ranges[i].lo - 1;
                k++;
            }
            if (ranges[i].hi == 0xFFFFFFFFu) {
                open = 0;
                break;
            }
            next = ranges[i].hi + 1;
        }
        if (open) {
            inv[k].lo = next;
            inv[k].hi = 0xFFFFFFFFu;
            k++;
        }
        for (int i = 0; i < k; i++) ranges[i] = inv[i];
        nranges = k;
    }
    return 0;
}

// Which range holds position pos (1-based), or -1.
static int range_of(uint32_t pos) {
    for (int i = 0; i < nranges; i++) {
        if (pos < ranges[i].lo) return -1;
        if (pos <= ranges[i].hi) return i;
    }
    return -1;
}

static void cut_bytes(const lio_line_t* l) {
    int last = -1;
    for (uint32_t i = 0; i < l->n; i++) {
        int r = range_of(i + 1);
        if (r < 0) continue;
        if (out_delim && last >= 0 && r != last) lio_put(&out, out_delim, out_delim_len);
        lio_putc(&out, l->s[i]);
        last = r;
    }
    lio_putc(&out, '\n');
}

static void cut_fields(const lio_line_t* l) {
    uint32_t i = 0;
    int has = 0;
    for (; i < l->n; i++)
        if (l->s[i] == delim) {
            has = 1;
            break;
        }
    if (!has) {
        if (!only_delim) {
            lio_put(&out, l->s, l->n);
            lio_putc(&out, '\n');
        }
        return;
    }
    uint32_t field = 1, start = 0;
    int printed = 0;
    for (i = 0;; i++) {
        if (i == l->n || l->s[i] == delim) {
            if (range_of(field) >= 0) {
                if (printed) {
                    if (out_delim) lio_put(&out, out_delim, out_delim_len);
                    else lio_putc(&out, delim);
                }
                lio_put(&out, l->s + start, i - start);
                printed = 1;
            }
            if (i == l->n) break;
            field++;
            start = i + 1;
        }
    }
    lio_putc(&out, '\n');
}

static int cut_fd(int fd, const char* name) {
    lio_in_t in;
    lio_in_init(&in, fd);
    lio_line_t l = { 0, 0, 0, 0 };
    while (lio_getline(&in, &out, &l)) {
        if (mode == 'f') cut_fields(&l);
        else cut_bytes(&l);
    }
    free(l.s);
    if (in.err) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "cut: %s: %s\n", name, strerror(in.err));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* list = 0;
    int nfiles = 0;
    char** files = argv + 1;
    int only_files = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_files || s[0] != '-' || !s[1]) {
            files[nfiles++] = argv[a];
            continue;
        }
        if (!strcmp(s, "--")) {
            only_files = 1;
            continue;
        }
        if (s[1] == '-') {
            if (!strcmp(s, "--complement")) complement = 1;
            else if (!strncmp(s, "--output-delimiter=", 19)) {
                out_delim = s + 19;
                out_delim_len = (uint32_t)strlen(out_delim);
                if (!out_delim_len) {  // an empty delimiter is a NUL byte, as in GNU cut
                    out_delim = "";
                    out_delim_len = 1;
                }
            } else {
                dprintf(STDERR_FILENO, "cut: unrecognized option '%s'\n", s);
                return usage();
            }
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 's') only_delim = 1;
            else if (*o == 'n') { }
            else if (*o == 'b' || *o == 'c' || *o == 'f' || *o == 'd') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (*o == 'd') {
                    if (strlen(v) != 1) {
                        dprintf(STDERR_FILENO, "cut: the delimiter must be a single character\n");
                        return 2;
                    }
                    delim = v[0];
                } else {
                    int m = *o == 'f' ? 'f' : 'b';
                    if (mode && (mode != m || list)) {
                        dprintf(STDERR_FILENO, "cut: only one type of list may be specified\n");
                        return usage();
                    }
                    mode = m;
                    list = v;
                }
                break;
            } else {
                dprintf(STDERR_FILENO, "cut: invalid option -- '%c'\n", *o);
                return usage();
            }
        }
    }
    if (!list) {
        dprintf(STDERR_FILENO, "cut: you must specify a list of bytes, characters, or fields\n");
        return usage();
    }
    if (mode != 'f' && only_delim) {
        dprintf(STDERR_FILENO, "cut: suppressing non-delimited lines makes sense only when operating on fields\n");
        return usage();
    }
    if (parse_list(list)) return 2;

    lio_out_init(&out, STDOUT_FILENO, "cut");
    int bad = 0;
    if (!nfiles) bad = cut_fd(STDIN_FILENO, "standard input");
    for (int i = 0; i < nfiles; i++) {
        if (!strcmp(files[i], "-")) {
            bad |= cut_fd(STDIN_FILENO, "standard input");
            continue;
        }
        int fd = sys_open(files[i], O_RDONLY);
        if (fd < 0) {
            lio_flush(&out);
            dprintf(STDERR_FILENO, "cut: %s: %s\n", files[i], strerror(fd));
            bad = 1;
            continue;
        }
        bad |= cut_fd(fd, files[i]);
        sys_close(fd);
    }
    lio_flush(&out);
    return bad;
}

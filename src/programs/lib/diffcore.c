// diff's engine (diffcore.h). The comparison follows GNU diffutils step
// by step, because the choice among equally short edit scripts is what
// people see:
//   1. The identical first and last lines are set aside (all but
//      `horizon` of them), byte for byte, flags or not.
//   2. The lines left are put into classes of lines that count as equal.
//   3. Lines that match nothing in the other file are marked changed at
//      once and left out of the search, and so, where they sit among
//      those, are lines that match too many.
//   4. Myers' search for a shortest edit script, by middle snakes, over
//      what is left.
//   5. Each run of changes slides as far down as it can, then back up to
//      meet a run in the other file if there is one.
// The names and order of the steps are GNU's (analyze.c, diffseq.h); the
// code is written for RingTail.
#include <stddef.h>
#include <stdint.h>
#include "diffcore.h"

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void free(void* p);
int memcmp(const void* a, const void* b, size_t n);

static int is_blank(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

int df_split(df_file_t* f, const char* buf, uint32_t len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] == '\n') n++;
    f->incomplete = len && buf[len - 1] != '\n';
    if (f->incomplete) n++;
    f->n = n;
    f->line = malloc((n ? n : 1) * sizeof *f->line);
    if (!f->line) return -1;
    uint32_t k = 0, start = 0;
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] == '\n') {
            f->line[k].s = buf + start;
            f->line[k++].len = i - start;
            start = i + 1;
        }
    if (f->incomplete) {
        f->line[k].s = buf + start;
        f->line[k].len = len - start;
    }
    return 0;
}

void df_free_file(df_file_t* f) {
    free(f->line);
    f->line = 0;
    f->n = 0;
}

// ---- equality under the flags ----

// The bytes of a line as the flags see them, one at a time.
typedef struct {
    const unsigned char *p, *end;
    unsigned flags;
} canon_t;

static int canon_next(canon_t* c) {  // -1 at the end
    if (c->flags & DF_ALLSPACE) {
        while (c->p < c->end && is_blank(*c->p)) c->p++;
    } else if ((c->flags & DF_SPACE) && c->p < c->end && is_blank(*c->p)) {
        while (c->p < c->end && is_blank(*c->p)) c->p++;
        if (c->p == c->end) return -1;  // blanks at the end do not count
        return ' ';
    }
    if (c->p == c->end) return -1;
    unsigned char ch = *c->p++;
    return (c->flags & DF_ICASE) ? lower(ch) : ch;
}

static uint32_t line_hash(const df_line_t* l, unsigned flags) {
    canon_t c = { (const unsigned char*)l->s, (const unsigned char*)l->s + l->len, flags };
    uint32_t h = 2166136261u;
    for (int ch; (ch = canon_next(&c)) >= 0;) h = (h ^ (uint32_t)ch) * 16777619u;
    return h;
}

static int lines_equal(const df_line_t* a, const df_line_t* b, unsigned flags) {
    if (a->len == b->len && !memcmp(a->s, b->s, a->len)) return 1;
    if (!(flags & (DF_ICASE | DF_SPACE | DF_ALLSPACE))) return 0;
    canon_t x = { (const unsigned char*)a->s, (const unsigned char*)a->s + a->len, flags };
    canon_t y = { (const unsigned char*)b->s, (const unsigned char*)b->s + b->len, flags };
    for (;;) {
        int p = canon_next(&x), q = canon_next(&y);
        if (p != q) return 0;
        if (p < 0) return 1;
    }
}

static int incomplete_at(const df_file_t* f, uint32_t i) { return f->incomplete && i == f->n - 1; }

// Byte-for-byte, as GNU finds the identical ends: a last line without its
// newline equals only another such line.
static int exact_equal(const df_file_t* f, uint32_t i, uint32_t j) {
    const df_line_t *a = &f[0].line[i], *b = &f[1].line[j];
    return a->len == b->len && incomplete_at(&f[0], i) == incomplete_at(&f[1], j) && !memcmp(a->s, b->s, a->len);
}

// ---- the search ----

typedef struct {
    const int32_t *xv, *yv;  // the class of each line still in play
    int32_t *fd, *bd;        // furthest x reached on each diagonal, both ways
    int32_t too_expensive;
    char *del, *ins;         // changed flags of each file's middle lines
    const int32_t *xreal, *yreal;
} ctx_t;

typedef struct {
    int32_t xmid, ymid;
    int lo_minimal, hi_minimal;
} part_t;

#define OFFSET_MAX 0x7FFFFFFF

// Finds the midpoint of a shortest edit script for x[xoff..xlim) to
// y[yoff..ylim), searching from both corners at once (Myers 1986, with
// GNU's order of trying diagonals, which decides ties).
static void diag(int32_t xoff, int32_t xlim, int32_t yoff, int32_t ylim, int find_minimal, part_t* part, ctx_t* c) {
    int32_t* const fd = c->fd;
    int32_t* const bd = c->bd;
    const int32_t* const xv = c->xv;
    const int32_t* const yv = c->yv;
    const int32_t dmin = xoff - ylim, dmax = xlim - yoff;
    const int32_t fmid = xoff - yoff, bmid = xlim - ylim;
    int32_t fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
    int odd = (fmid - bmid) & 1;
    fd[fmid] = xoff;
    bd[bmid] = xlim;
    for (int32_t cost = 1;; cost++) {
        int32_t d;
        if (fmin > dmin) fd[--fmin - 1] = -1;
        else ++fmin;
        if (fmax < dmax) fd[++fmax + 1] = -1;
        else --fmax;
        for (d = fmax; d >= fmin; d -= 2) {
            int32_t tlo = fd[d - 1], thi = fd[d + 1];
            int32_t x0 = tlo < thi ? thi : tlo + 1, x, y;
            for (x = x0, y = x0 - d; x < xlim && y < ylim && xv[x] == yv[y]; x++, y++) { }
            fd[d] = x;
            if (odd && bmin <= d && d <= bmax && bd[d] <= x) {
                part->xmid = x;
                part->ymid = y;
                part->lo_minimal = part->hi_minimal = 1;
                return;
            }
        }
        if (bmin > dmin) bd[--bmin - 1] = OFFSET_MAX;
        else ++bmin;
        if (bmax < dmax) bd[++bmax + 1] = OFFSET_MAX;
        else --bmax;
        for (d = bmax; d >= bmin; d -= 2) {
            int32_t tlo = bd[d - 1], thi = bd[d + 1];
            int32_t x0 = tlo < thi ? tlo : thi - 1, x, y;
            for (x = x0, y = x0 - d; xoff < x && yoff < y && xv[x - 1] == yv[y - 1]; x--, y--) { }
            bd[d] = x;
            if (!odd && fmin <= d && d <= fmax && x <= fd[d]) {
                part->xmid = x;
                part->ymid = y;
                part->lo_minimal = part->hi_minimal = 1;
                return;
            }
        }
        if (find_minimal) continue;
        // Gone well past the call of duty: settle for the better of the
        // furthest points reached so far.
        if (cost >= c->too_expensive) {
            int32_t fxybest = -1, fxbest = 0, bxybest = OFFSET_MAX, bxbest = 0;
            for (d = fmax; d >= fmin; d -= 2) {
                int32_t x = fd[d] < xlim ? fd[d] : xlim, y = x - d;
                if (ylim < y) {
                    x = ylim + d;
                    y = ylim;
                }
                if (fxybest < x + y) {
                    fxybest = x + y;
                    fxbest = x;
                }
            }
            for (d = bmax; d >= bmin; d -= 2) {
                int32_t x = bd[d] > xoff ? bd[d] : xoff, y = x - d;
                if (y < yoff) {
                    x = yoff + d;
                    y = yoff;
                }
                if (x + y < bxybest) {
                    bxybest = x + y;
                    bxbest = x;
                }
            }
            if ((xlim + ylim) - bxybest < fxybest - (xoff + yoff)) {
                part->xmid = fxbest;
                part->ymid = fxybest - fxbest;
                part->lo_minimal = 1;
                part->hi_minimal = 0;
            } else {
                part->xmid = bxbest;
                part->ymid = bxybest - bxbest;
                part->lo_minimal = 0;
                part->hi_minimal = 1;
            }
            return;
        }
    }
}

// Marks the lines of x[xoff..xlim) and y[yoff..ylim) that a shortest
// edit script deletes and inserts. Recurses into the smaller half and
// loops on the other, so the depth stays near log2 of the size.
static void compareseq(int32_t xoff, int32_t xlim, int32_t yoff, int32_t ylim, int find_minimal, ctx_t* c) {
    for (;;) {
        while (xoff < xlim && yoff < ylim && c->xv[xoff] == c->yv[yoff]) xoff++, yoff++;
        while (xoff < xlim && yoff < ylim && c->xv[xlim - 1] == c->yv[ylim - 1]) xlim--, ylim--;
        if (xoff == xlim) {
            for (; yoff < ylim; yoff++) c->ins[c->yreal[yoff]] = 1;
            return;
        }
        if (yoff == ylim) {
            for (; xoff < xlim; xoff++) c->del[c->xreal[xoff]] = 1;
            return;
        }
        part_t p;
        diag(xoff, xlim, yoff, ylim, find_minimal, &p, c);
        if ((xlim + ylim) - (p.xmid + p.ymid) < (p.xmid + p.ymid) - (xoff + yoff)) {
            compareseq(p.xmid, xlim, p.ymid, ylim, p.hi_minimal, c);
            xlim = p.xmid;
            ylim = p.ymid;
            find_minimal = p.lo_minimal;
        } else {
            compareseq(xoff, p.xmid, yoff, p.ymid, p.lo_minimal, c);
            xoff = p.xmid;
            yoff = p.ymid;
            find_minimal = p.hi_minimal;
        }
    }
}

// ---- before and after the search ----

typedef struct {
    uint32_t n;           // lines in play (the middle)
    int32_t* equiv;       // class of each
    char* changed;        // [-1] and [n] exist and stay 0
    int32_t* undiscarded; // the classes the search sees
    int32_t* real;        // and which line each one is
    uint32_t nkept;
} side_t;

// Step 3 (GNU's discard_confusing_lines): a line that matches nothing in
// the other file is certainly changed; one that matches many lines is
// left out too when it sits among such lines, which keeps the search from
// pairing up lines that are only alike by accident.
static void discard_confusing(side_t s[2], int32_t nclasses, int minimal, char* space) {
    uint32_t* count[2];
    count[0] = calloc((size_t)nclasses * 2, sizeof(uint32_t));
    char* discarded[2] = { space, space + s[0].n };
    if (count[0]) {
        count[1] = count[0] + nclasses;
        for (int f = 0; f < 2; f++)
            for (uint32_t i = 0; i < s[f].n; i++) count[f][s[f].equiv[i]]++;
        for (int f = 0; f < 2; f++) {
            uint32_t end = s[f].n, many = 5, tem = end / 64;
            while ((tem >>= 2) > 0) many *= 2;
            for (uint32_t i = 0; i < end; i++) {
                uint32_t nmatch = count[1 - f][s[f].equiv[i]];
                discarded[f][i] = nmatch == 0 ? 1 : nmatch > many ? 2 : 0;
            }
        }
        free(count[0]);
        for (int f = 0; f < 2; f++) {
            int32_t end = (int32_t)s[f].n;
            char* d = discarded[f];
            for (int32_t i = 0; i < end; i++) {
                if (d[i] == 2) {
                    d[i] = 0;  // a provisional discard not inside a run
                    continue;
                }
                if (!d[i]) continue;
                // A run of discardable lines starting with a certain one.
                int32_t j, provisional = 0;
                for (j = i; j < end && d[j]; j++)
                    if (d[j] == 2) provisional++;
                while (j > i && d[j - 1] == 2) d[--j] = 0, provisional--;
                int32_t length = j - i;
                if (provisional * 4 > length) {
                    while (j > i)
                        if (d[--j] == 2) d[j] = 0;
                } else {
                    int32_t minimum = 1, t = length >> 2, consec;
                    while ((t >>= 2) > 0) minimum <<= 1;
                    minimum++;
                    // Keep (cancel the discard of) any stretch of MINIMUM or
                    // more provisional lines inside the run.
                    for (j = 0, consec = 0; j < length; j++)
                        if (d[i + j] != 2) consec = 0;
                        else if (minimum == ++consec) j -= consec;
                        else if (minimum < consec) d[i + j] = 0;
                    // From each end, keep the provisional ones until three
                    // certain ones in a row, or the first certain one 8 in.
                    for (j = 0, consec = 0; j < length; j++) {
                        if (j >= 8 && d[i + j] == 1) break;
                        if (d[i + j] == 2) consec = 0, d[i + j] = 0;
                        else if (d[i + j] == 0) consec = 0;
                        else consec++;
                        if (consec == 3) break;
                    }
                    i += length - 1;
                    for (j = 0, consec = 0; j < length; j++) {
                        if (j >= 8 && d[i - j] == 1) break;
                        if (d[i - j] == 2) consec = 0, d[i - j] = 0;
                        else if (d[i - j] == 0) consec = 0;
                        else consec++;
                        if (consec == 3) break;
                    }
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < s[0].n + s[1].n; i++) space[i] = 0;  // no memory: discard nothing
    }
    for (int f = 0; f < 2; f++) {
        uint32_t k = 0;
        for (uint32_t i = 0; i < s[f].n; i++)
            if (minimal || !discarded[f][i]) {
                s[f].undiscarded[k] = s[f].equiv[i];
                s[f].real[k++] = (int32_t)i;
            } else {
                s[f].changed[i] = 1;
            }
        s[f].nkept = k;
    }
}

// Step 5 (GNU's shift_boundaries): slide each run of changed lines over
// equal neighbours, down as far as it goes (merging with runs it meets),
// then back up to end where a run in the other file ends, if one does.
static void shift_boundaries(side_t s[2]) {
    for (int f = 0; f < 2; f++) {
        char* changed = s[f].changed;
        const char* other = s[1 - f].changed;
        const int32_t* equiv = s[f].equiv;
        int32_t i = 0, j = 0, i_end = (int32_t)s[f].n;
        for (;;) {
            int32_t runlength, start, corresponding;
            while (i < i_end && !changed[i]) {
                while (other[j++]) { }
                i++;
            }
            if (i == i_end) break;
            start = i;
            while (changed[++i]) { }
            while (other[j]) j++;
            do {
                runlength = i - start;
                while (start && equiv[start - 1] == equiv[i - 1]) {
                    changed[--start] = 1;
                    changed[--i] = 0;
                    while (changed[start - 1]) start--;
                    while (other[--j]) { }
                }
                corresponding = other[j - 1] ? i : i_end;
                while (i != i_end && equiv[start] == equiv[i]) {
                    changed[start++] = 0;
                    changed[i++] = 1;
                    while (changed[i]) i++;
                    while (other[++j]) corresponding = i;
                }
            } while (runlength != i - start);
            while (corresponding < i) {
                changed[--start] = 1;
                changed[--i] = 0;
                while (other[--j]) { }
            }
        }
    }
}

// Step 2: numbers the classes of equal lines from 1, over both files'
// middles. A last line without a newline is a class of its own (shared
// only with the other file's such line) unless -b or -w make the newline
// not count.
typedef struct {
    uint32_t hash;
    int32_t next;
    const df_line_t* line;
    int incomplete;
} class_t;

static int32_t classify(const df_file_t f[2], uint32_t first, side_t s[2], unsigned flags) {
    uint32_t total = s[0].n + s[1].n;
    uint32_t nb = 16;
    while (nb < total * 2) nb <<= 1;
    int32_t* bucket = calloc(nb, sizeof *bucket);
    class_t* cls = malloc((total + 1) * sizeof *cls);
    if (!bucket || !cls) {
        free(bucket);
        free(cls);
        return -1;
    }
    int32_t ncls = 1;
    int nl_counts = !(flags & (DF_SPACE | DF_ALLSPACE));
    for (int k = 0; k < 2; k++)
        for (uint32_t i = 0; i < s[k].n; i++) {
            const df_line_t* l = &f[k].line[first + i];
            int inc = nl_counts && incomplete_at(&f[k], first + i);
            uint32_t h = line_hash(l, flags);
            int32_t* b = &bucket[(h ^ (uint32_t)inc) & (nb - 1)];
            int32_t c;
            for (c = *b; c; c = cls[c].next)
                if (cls[c].hash == h && cls[c].incomplete == inc && lines_equal(cls[c].line, l, flags)) break;
            if (!c) {
                c = ncls++;
                cls[c].hash = h;
                cls[c].line = l;
                cls[c].incomplete = inc;
                cls[c].next = *b;
                *b = c;
            }
            s[k].equiv[i] = c;
        }
    free(bucket);
    free(cls);
    return ncls;
}

static df_change_t* add_change(uint32_t l0, uint32_t l1, uint32_t del, uint32_t ins, df_change_t* next) {
    df_change_t* c = malloc(sizeof *c);
    if (!c) return 0;
    c->line0 = l0;
    c->line1 = l1;
    c->deleted = del;
    c->inserted = ins;
    c->link = next;
    return c;
}

void df_free_script(df_change_t* s) {
    while (s) {
        df_change_t* n = s->link;
        free(s);
        s = n;
    }
}

int df_compare(const df_file_t f[2], unsigned flags, uint32_t horizon, df_change_t** out) {
    *out = 0;
    uint32_t n0 = f[0].n, n1 = f[1].n;
    // Step 1: the identical start and end, less the horizon.
    uint32_t k = 0;
    while (k < n0 && k < n1 && exact_equal(f, k, k)) k++;
    uint32_t prefix = k > horizon ? k - horizon : 0;
    uint32_t s = 0, room = (n0 < n1 ? n0 : n1) - prefix;
    while (s < room && exact_equal(f, n0 - 1 - s, n1 - 1 - s)) s++;
    uint32_t suffix = s > horizon ? s - horizon : 0;

    side_t sd[2];
    sd[0].n = n0 - prefix - suffix;
    sd[1].n = n1 - prefix - suffix;
    uint32_t m = sd[0].n + sd[1].n;
    int32_t* ints = malloc((size_t)(m * 3 + 1) * sizeof *ints);
    char* flagspace = calloc(m + 4, 1);
    char* discspace = malloc(m + 1);
    int32_t* diags = 0;
    int r = -1;
    if (!ints || !flagspace || !discspace) goto done;
    sd[0].equiv = ints;
    sd[1].equiv = ints + sd[0].n;
    sd[0].undiscarded = ints + m;
    sd[1].undiscarded = ints + m + sd[0].n;
    sd[0].real = ints + 2 * m;
    sd[1].real = ints + 2 * m + sd[0].n;
    sd[0].changed = flagspace + 1;
    sd[1].changed = flagspace + sd[0].n + 3;

    int32_t ncls = classify(f, prefix, sd, flags);
    if (ncls < 0) goto done;
    discard_confusing(sd, ncls, (flags & DF_MINIMAL) != 0, discspace);

    // Step 4.
    uint32_t nd = sd[0].nkept + sd[1].nkept + 3;
    diags = malloc((size_t)nd * 2 * sizeof *diags);
    if (!diags) goto done;
    ctx_t c;
    c.xv = sd[0].undiscarded;
    c.yv = sd[1].undiscarded;
    c.fd = diags + sd[1].nkept + 1;
    c.bd = diags + nd + sd[1].nkept + 1;
    c.del = sd[0].changed;
    c.ins = sd[1].changed;
    c.xreal = sd[0].real;
    c.yreal = sd[1].real;
    int32_t te = 1;
    for (uint32_t d = nd; d; d >>= 2) te <<= 1;
    c.too_expensive = te > 4096 ? te : 4096;
    compareseq(0, (int32_t)sd[0].nkept, 0, (int32_t)sd[1].nkept, (flags & DF_MINIMAL) != 0, &c);

    shift_boundaries(sd);

    // The script, built from the end so it comes out in order.
    {
        df_change_t* script = 0;
        int32_t i0 = (int32_t)sd[0].n, i1 = (int32_t)sd[1].n;
        const char *c0 = sd[0].changed, *c1 = sd[1].changed;
        while (i0 >= 0 || i1 >= 0) {
            if (c0[i0 - 1] | c1[i1 - 1]) {
                int32_t l0 = i0, l1 = i1;
                while (c0[i0 - 1]) i0--;
                while (c1[i1 - 1]) i1--;
                df_change_t* ch = add_change(prefix + (uint32_t)i0, prefix + (uint32_t)i1, (uint32_t)(l0 - i0), (uint32_t)(l1 - i1), script);
                if (!ch) {
                    df_free_script(script);
                    goto done;
                }
                script = ch;
            }
            i0--, i1--;
        }
        *out = script;
    }
    r = 0;
done:
    free(ints);
    free(flagspace);
    free(discspace);
    free(diags);
    return r;
}

// ---- printing ----

typedef struct {
    df_write_t write;
    void* ctx;
} out_t;

static void put(out_t* o, const char* s, uint32_t n) { o->write(o->ctx, s, n); }
static void puts_(out_t* o, const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    put(o, s, n);
}
static void putnum(out_t* o, uint32_t v) {
    char b[12];
    int i = 12;
    do b[--i] = (char)('0' + v % 10); while (v /= 10);
    put(o, b + i, (uint32_t)(12 - i));
}

// A line after its marker, its newline, and the note GNU adds after a
// last line that had none.
static void put_line(out_t* o, const df_file_t* f, uint32_t i) {
    put(o, f->line[i].s, f->line[i].len);
    put(o, "\n", 1);
    if (incomplete_at(f, i)) puts_(o, "\\ No newline at end of file\n");
}

// Ranges are given as first..last, 0-based, last = first - 1 when empty.
static void range_normal(out_t* o, int64_t a, int64_t b) {
    if (b > a) {  // more than one line: "A,B"
        putnum(o, (uint32_t)(a + 1));
        put(o, ",", 1);
    }
    putnum(o, (uint32_t)(b + 1));  // else the one line, or the line before an empty range
}

static void range_unified(out_t* o, int64_t a, int64_t b) {
    if (b < a) {
        putnum(o, (uint32_t)(b + 1));
        puts_(o, ",0");
    } else if (b == a) {
        putnum(o, (uint32_t)(a + 1));
    } else {
        putnum(o, (uint32_t)(a + 1));
        put(o, ",", 1);
        putnum(o, (uint32_t)(b - a + 1));
    }
}

void df_print_normal(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* c) {
    out_t o = { write, ctx };
    for (; c; c = c->link) {
        int64_t f0 = c->line0, l0 = (int64_t)c->line0 + c->deleted - 1;
        int64_t f1 = c->line1, l1 = (int64_t)c->line1 + c->inserted - 1;
        range_normal(&o, f0, l0);
        put(&o, c->deleted && c->inserted ? "c" : c->deleted ? "d" : "a", 1);
        range_normal(&o, f1, l1);
        put(&o, "\n", 1);
        for (uint32_t i = 0; i < c->deleted; i++) {
            put(&o, "< ", 2);
            put_line(&o, &f[0], c->line0 + i);
        }
        if (c->deleted && c->inserted) put(&o, "---\n", 4);
        for (uint32_t i = 0; i < c->inserted; i++) {
            put(&o, "> ", 2);
            put_line(&o, &f[1], c->line1 + i);
        }
    }
}

// The last change of the hunk that starts at c: changes join while the
// unchanged stretch between them is at most twice the context.
static const df_change_t* hunk_end(const df_change_t* c, uint32_t context) {
    for (;;) {
        uint32_t top0 = c->line0 + c->deleted;
        const df_change_t* n = c->link;
        if (!n || (uint64_t)(n->line0 - top0) >= 2 * (uint64_t)context + 1) return c;
        c = n;
    }
}

// The lines of each file a hunk shows, context included.
static void hunk_span(const df_file_t f[2], const df_change_t* a, const df_change_t* z, uint32_t context,
                      int64_t* first0, int64_t* last0, int64_t* first1, int64_t* last1) {
    int64_t f0 = (int64_t)a->line0 - context, f1 = (int64_t)a->line1 - context;
    int64_t l0 = (int64_t)z->line0 + z->deleted - 1 + context, l1 = (int64_t)z->line1 + z->inserted - 1 + context;
    *first0 = f0 < 0 ? 0 : f0;
    *first1 = f1 < 0 ? 0 : f1;
    *last0 = l0 > (int64_t)f[0].n - 1 ? (int64_t)f[0].n - 1 : l0;
    *last1 = l1 > (int64_t)f[1].n - 1 ? (int64_t)f[1].n - 1 : l1;
}

void df_print_unified(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* c, uint32_t context) {
    out_t o = { write, ctx };
    while (c) {
        const df_change_t* z = hunk_end(c, context);
        int64_t first0, last0, first1, last1;
        hunk_span(f, c, z, context, &first0, &last0, &first1, &last1);
        puts_(&o, "@@ -");
        range_unified(&o, first0, last0);
        puts_(&o, " +");
        range_unified(&o, first1, last1);
        puts_(&o, " @@\n");
        int64_t i = first0, j = first1;
        const df_change_t* next = c;
        const df_change_t* stop = z->link;
        while (i <= last0 || j <= last1) {
            if (next == stop || i < next->line0) {
                put(&o, " ", 1);
                put_line(&o, &f[0], (uint32_t)i++);
                j++;
            } else {
                for (uint32_t k = 0; k < next->deleted; k++) {
                    put(&o, "-", 1);
                    put_line(&o, &f[0], (uint32_t)i++);
                }
                for (uint32_t k = 0; k < next->inserted; k++) {
                    put(&o, "+", 1);
                    put_line(&o, &f[1], (uint32_t)j++);
                }
                next = next->link;
            }
        }
        c = stop;
    }
}

static void range_context(out_t* o, int64_t a, int64_t b) {
    if (b <= a) {
        putnum(o, (uint32_t)(b + 1));
    } else {
        putnum(o, (uint32_t)(a + 1));
        put(o, ",", 1);
        putnum(o, (uint32_t)(b + 1));
    }
}

void df_print_context(df_write_t write, void* ctx, const df_file_t f[2], const df_change_t* c, uint32_t context) {
    out_t o = { write, ctx };
    while (c) {
        const df_change_t* z = hunk_end(c, context);
        const df_change_t* stop = z->link;
        int64_t first0, last0, first1, last1;
        hunk_span(f, c, z, context, &first0, &last0, &first1, &last1);
        int old = 0, new_ = 0;
        for (const df_change_t* e = c; e != stop; e = e->link) {
            if (e->deleted) old = 1;
            if (e->inserted) new_ = 1;
        }
        puts_(&o, "***************\n*** ");
        range_context(&o, first0, last0);
        puts_(&o, " ****\n");
        if (old) {
            const df_change_t* next = c;
            for (int64_t i = first0; i <= last0; i++) {
                while (next != stop && next->line0 + next->deleted <= i) next = next->link;
                const char* mark = "  ";
                if (next != stop && next->line0 <= i) mark = next->inserted ? "! " : "- ";
                put(&o, mark, 2);
                put_line(&o, &f[0], (uint32_t)i);
            }
        }
        puts_(&o, "--- ");
        range_context(&o, first1, last1);
        puts_(&o, " ----\n");
        if (new_) {
            const df_change_t* next = c;
            for (int64_t j = first1; j <= last1; j++) {
                while (next != stop && next->line1 + next->inserted <= j) next = next->link;
                const char* mark = "  ";
                if (next != stop && next->line1 <= j) mark = next->deleted ? "! " : "+ ";
                put(&o, mark, 2);
                put_line(&o, &f[1], (uint32_t)j);
            }
        }
        c = stop;
    }
}

// comm [-123] [--check-order | --nocheck-order] [--output-delimiter=STR]
//      [--total] FILE1 FILE2
//
// Compares two sorted files line by line and writes three columns: the
// lines only in FILE1, the lines only in FILE2, and the lines in both.
// A column is moved in by one tab for each column before it that is
// shown. "-" is standard input (for one of the two). The files must be
// sorted bytewise, as `sort` does; a line counts as in both files once
// for each pair, so a line that is twice in one file and once in the
// other is in both once and only in the first once.
//   -1 -2 -3            leave a column out (-12 shows only the third)
//   --output-delimiter  use STR instead of the tab; empty means a NUL byte
//   --total             after the lines, write the three counts and "total"
//                       (the counts include the columns that are left out)
//   --check-order       stop with an error at the first line out of order
//   --nocheck-order     never check
// Without either, the order is only checked once the two files have
// disagreed about a line, and a line out of order is reported (once for
// each file, as "comm: file N is not in sorted order") while the work
// goes on; the end result is then an error status. As in GNU comm, a
// line is checked against the one before it when it is read, and when a
// file ends its last two lines are checked again, so a disorder that
// was read before the files first disagreed shows up only if it is at
// the very end. A long option may be
// shortened as long as it stays unique. Both names may not be "-". A last
// line with no newline gets one. There is no -z.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO,
            "usage: comm [-123] [--check-order | --nocheck-order] [--output-delimiter=STR] [--total] FILE1 FILE2\n");
    return 2;
}

enum { CHECK_DEFAULT, CHECK_ON, CHECK_OFF };

static lio_out_t out;
static int order_mode = CHECK_DEFAULT;
static int hide[3];  // columns left out
static const char* delim = "\t";
static uint32_t dlen = 1;
static int seen_unpairable;
static int warned[2];

typedef struct {
    lio_in_t in;
    const char* name;
    lio_line_t cur, prev, pp;  // this line, the one before, the one before that
    int have;       // cur is a line
    int nread;      // how many lines have moved on into prev
} input_t;

static input_t inp[2];

static int bad;

// -1, 0 or 1 as a sorts before, with or after b, bytewise.
static int compare(const lio_line_t* a, const lio_line_t* b) {
    uint32_t n = a->n < b->n ? a->n : b->n;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a->s[i], y = (unsigned char)b->s[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return a->n < b->n ? -1 : a->n > b->n;
}

// Whether `second`, which came after `first` in input `which`, is out of order.
static void check_order(const lio_line_t* first, const lio_line_t* second, int which) {
    if (order_mode == CHECK_OFF || (order_mode == CHECK_DEFAULT && !seen_unpairable)) return;
    if (warned[which] || compare(first, second) <= 0) return;
    lio_flush(&out);
    dprintf(STDERR_FILENO, "comm: file %d is not in sorted order\n", which + 1);
    warned[which] = 1;
    if (order_mode == CHECK_ON) sys_exit(1);
}

// Moves input `which` on to its next line.
static void advance(int which) {
    input_t* f = &inp[which];
    if (f->have) {
        lio_line_t t = f->pp;
        f->pp = f->prev;
        f->prev = f->cur;
        f->cur = t;
        f->nread++;
    }
    f->have = lio_getline(&f->in, &out, &f->cur);
    if (f->have) {
        if (f->nread >= 1) check_order(&f->prev, &f->cur, which);
        return;
    }
    if (f->nread >= 2) check_order(&f->pp, &f->prev, which);
    if (f->in.err) {
        lio_flush(&out);
        dprintf(STDERR_FILENO, "comm: %s: %s\n", f->name, strerror(f->in.err));
        bad = 1;
        f->in.err = 0;
    }
}

// The line in column col (1 to 3), if that column is shown.
static void write_line(int col, const lio_line_t* l) {
    if (hide[col - 1]) return;
    for (int c = 1; c < col; c++)
        if (!hide[c - 1]) lio_put(&out, delim, dlen);
    lio_put(&out, l->s, l->n);
    lio_putc(&out, '\n');
}

static void put_count(uint64_t v) {
    char d[24];
    int nd = 0;
    do {
        d[nd++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (nd) lio_putc(&out, d[--nd]);
}

static int long_option(const char* s, int* total) {
    static const char* names[] = { "check-order", "nocheck-order", "output-delimiter", "total" };
    const char* eq = strchr(s, '=');
    uint32_t n = eq ? (uint32_t)(eq - s) : (uint32_t)strlen(s);
    int hit = -1, hits = 0;
    for (int i = 0; i < 4; i++) {
        if (n <= strlen(names[i]) && !strncmp(s, names[i], n)) {
            if (n == strlen(names[i])) {
                hit = i;
                hits = 1;
                break;
            }
            hit = i;
            hits++;
        }
    }
    if (hits != 1) return -1;
    if (hit == 2) {
        if (!eq) return -1;
        eq++;
        static const char nul[1] = { 0 };
        const char* want = *eq ? eq : nul;
        uint32_t wl = *eq ? (uint32_t)strlen(eq) : 1;
        static int given;
        if (given && (wl != dlen || memcmp(want, delim, wl))) {
            dprintf(STDERR_FILENO, "comm: multiple output delimiters specified\n");
            sys_exit(1);
        }
        given = 1;
        delim = want;
        dlen = wl;
        return 0;
    }
    if (eq) return -1;
    if (hit == 0) order_mode = CHECK_ON;
    else if (hit == 1) order_mode = CHECK_OFF;
    else *total = 1;
    return 0;
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0, total = 0;
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
            if (long_option(s + 2, &total)) return usage();
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o < '1' || *o > '3') return usage();
            hide[*o - '1'] = 1;
        }
    }
    if (nops != 2) return usage();
    if (!strcmp(argv[1], "-") && !strcmp(argv[2], "-")) {
        dprintf(STDERR_FILENO, "comm: only one of the files may be standard input\n");
        return 1;
    }

    int fd[2];
    for (int i = 0; i < 2; i++) {
        const char* a = argv[1 + i];
        inp[i].name = strcmp(a, "-") ? a : "standard input";
        fd[i] = strcmp(a, "-") ? sys_open(a, O_RDONLY) : STDIN_FILENO;
        if (fd[i] < 0) {
            fs_error("comm", a, fd[i]);
            return 1;
        }
        lio_in_init(&inp[i].in, fd[i]);
        inp[i].cur = (lio_line_t){ 0, 0, 0, 0 };
        inp[i].prev = (lio_line_t){ 0, 0, 0, 0 };
        inp[i].pp = (lio_line_t){ 0, 0, 0, 0 };
    }

    lio_out_init(&out, STDOUT_FILENO, "comm");
    uint64_t only1 = 0, only2 = 0, both = 0;
    advance(0);
    advance(1);
    while (inp[0].have || inp[1].have) {
        int order = !inp[0].have ? 1 : !inp[1].have ? -1 : compare(&inp[0].cur, &inp[1].cur);
        if (order) seen_unpairable = 1;
        if (order < 0) {
            only1++;
            write_line(1, &inp[0].cur);
            advance(0);
        } else if (order > 0) {
            only2++;
            write_line(2, &inp[1].cur);
            advance(1);
        } else {
            both++;
            write_line(3, &inp[1].cur);
            advance(0);
            advance(1);
        }
    }
    if (total) {
        put_count(only1);
        lio_put(&out, delim, dlen);
        put_count(only2);
        lio_put(&out, delim, dlen);
        put_count(both);
        lio_put(&out, delim, dlen);
        lio_puts(&out, "total\n");
    }
    lio_flush(&out);
    if (warned[0] || warned[1]) {
        dprintf(STDERR_FILENO, "comm: input is not in sorted order\n");
        return 1;
    }
    return bad;
}

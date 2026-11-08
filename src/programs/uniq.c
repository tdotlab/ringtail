// uniq [-c | -d | -u] [-i] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]
//
// Collapses runs of equal adjacent lines, from INPUT (or standard input,
// or "-") to OUTPUT (or standard output), keeping the first line of each
// run. Run `sort` first to bring equal lines together.
//   -c  put the number of times each line came in front of it
//   -d  only lines that came more than once (one of each)
//   -u  only lines that came exactly once
//   -i  ignore case
//   -f N  skip the first N fields when comparing (a field is blanks
//         followed by non-blanks); -s N then skips N more characters
//   -w N  compare no more than N characters after that
// Options may come after the operands; "--" ends them. A last line with
// no newline is written with one.
//
// Output is flushed whenever uniq is about to wait for more input, so
// at the end of a slow pipeline lines show up as they are decided.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: uniq [-cdui] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]\n");
    return 2;
}

static int opt_c, opt_d, opt_u, opt_i;
static uint32_t skip_fields, skip_chars, width = 0xFFFFFFFFu;

typedef struct {
    char* s;
    uint32_t n, cap;
} line_t;

static int in_fd = STDIN_FILENO, out_fd = STDOUT_FILENO;
static char ib[1024];
static int ilen, ipos, read_err;
static char ob[1024];
static uint32_t on;

static void flush(void) {
    const char* p = ob;
    while (on) {
        int w = sys_write(out_fd, p, on);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "uniq: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
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

static void oom(void) {
    dprintf(STDERR_FILENO, "uniq: out of memory\n");
    sys_exit(1);
}

static void put_char(line_t* l, char c) {
    if (l->n == l->cap) {
        uint32_t want = l->cap ? l->cap * 2 : 256;
        if (want > 0x20000000u) oom();
        char* p = realloc(l->s, want);
        if (!p) oom();
        l->s = p;
        l->cap = want;
    }
    l->s[l->n++] = c;
}

// The next line, without its newline. 0 at the end of the input.
static int read_line(line_t* l) {
    l->n = 0;
    int got = 0;
    for (;;) {
        if (ipos == ilen) {
            flush();
            ilen = sys_read(in_fd, ib, sizeof ib);
            ipos = 0;
            if (ilen <= 0) {
                if (ilen < 0) read_err = ilen;
                ilen = 0;
                return got;
            }
        }
        got = 1;
        char c = ib[ipos++];
        if (c == '\n') return 1;
        put_char(l, c);
    }
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

// The part of l that is compared.
static void key_of(const line_t* l, const char** p, uint32_t* n) {
    uint32_t i = 0;
    for (uint32_t f = 0; f < skip_fields; f++) {
        while (i < l->n && is_blank(l->s[i])) i++;
        while (i < l->n && !is_blank(l->s[i])) i++;
    }
    uint32_t rest = l->n - i;
    i += skip_chars < rest ? skip_chars : rest;
    rest = l->n - i;
    *p = l->s + i;
    *n = rest < width ? rest : width;
}

static unsigned char low(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static int same(const line_t* a, const line_t* b) {
    const char *ap, *bp;
    uint32_t an, bn;
    key_of(a, &ap, &an);
    key_of(b, &bp, &bn);
    if (an != bn) return 0;
    if (!opt_i) return memcmp(ap, bp, an) == 0;
    for (uint32_t i = 0; i < an; i++)
        if (low((unsigned char)ap[i]) != low((unsigned char)bp[i])) return 0;
    return 1;
}

static void report(const line_t* l, uint32_t count) {
    if (opt_d && count < 2) return;
    if (opt_u && count > 1) return;
    if (opt_c) {
        char num[16];
        int len = snprintf(num, sizeof num, "%7u ", count);
        emit(num, (uint32_t)len);
    }
    emit(l->s, l->n);
    emit("\n", 1);
}

static int number(const char* v, uint32_t* out, const char* what) {
    if (parse_uint(v, out)) {
        dprintf(STDERR_FILENO, "uniq: invalid number of %s: '%s'\n", what, v);
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* ops[2];
    int nops = 0, only_ops = 0;
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        if (only_ops || s[0] != '-' || !s[1]) {
            if (nops == 2) {
                dprintf(STDERR_FILENO, "uniq: extra operand '%s'\n", s);
                return usage();
            }
            ops[nops++] = s;
            continue;
        }
        if (!strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'c') opt_c = 1;
            else if (*o == 'd') opt_d = 1;
            else if (*o == 'u') opt_u = 1;
            else if (*o == 'i') opt_i = 1;
            else if (*o == 'f' || *o == 's' || *o == 'w') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (*o == 'f' && number(v, &skip_fields, "fields to skip")) return 2;
                if (*o == 's' && number(v, &skip_chars, "characters to skip")) return 2;
                if (*o == 'w' && number(v, &width, "characters to compare")) return 2;
                break;
            } else {
                return usage();
            }
        }
    }

    const char* name = "standard input";
    if (nops > 0 && strcmp(ops[0], "-")) {
        name = ops[0];
        in_fd = sys_open(name, O_RDONLY);
        if (in_fd < 0) {
            dprintf(STDERR_FILENO, "uniq: %s: %s\n", name, strerror(in_fd));
            return 1;
        }
    }
    if (nops > 1 && strcmp(ops[1], "-")) {
        out_fd = sys_open(ops[1], O_WRONLY | O_CREAT | O_TRUNC);
        if (out_fd < 0) {
            dprintf(STDERR_FILENO, "uniq: %s: %s\n", ops[1], strerror(out_fd));
            return 1;
        }
    }

    line_t prev = { 0, 0, 0 }, cur = { 0, 0, 0 };
    int have = 0;
    uint32_t count = 0;
    while (read_line(&cur)) {
        if (have && same(&prev, &cur)) {
            count++;
            continue;
        }
        if (have) report(&prev, count);
        line_t t = prev;
        prev = cur;
        cur = t;
        have = 1;
        count = 1;
    }
    if (have) report(&prev, count);
    flush();
    if (read_err) {
        dprintf(STDERR_FILENO, "uniq: %s: %s\n", name, strerror(read_err));
        return 1;
    }
    return 0;
}

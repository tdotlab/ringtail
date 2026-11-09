// cmp [-l] [-s] [-n LIMIT] FILE1 [FILE2]
//
// Compares two files byte by byte (FILE2 defaults to standard input; "-"
// is standard input too). Prints nothing and exits 0 if they are the
// same; otherwise says where they first differ, "a b differ: byte N,
// line M", and exits 1.
//   -l  print every differing byte: its number (decimal) and the two
//       values (octal), one line each
//   -s  print nothing, only the exit status
//   -n  compare at most LIMIT bytes
// If one file ends first, "cmp: EOF on FILE after byte N" goes to
// standard error (and the exit status is 1). Exit status 2 means trouble.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: cmp [-l] [-s] [-n LIMIT] FILE1 [FILE2]\n");
    return 2;
}

typedef struct {
    int fd;
    const char* name;
    char buf[1024];
    int len, pos;
    int err;
} src_t;

static int next(src_t* s) {
    if (s->pos == s->len) {
        if (s->err) return -1;
        s->len = sys_read(s->fd, s->buf, sizeof s->buf);
        s->pos = 0;
        if (s->len <= 0) {
            if (s->len < 0) s->err = s->len;
            s->len = 0;
            return -1;
        }
    }
    return (unsigned char)s->buf[s->pos++];
}

static int open_src(src_t* s, const char* name, uint32_t* size) {
    s->name = name;
    s->len = s->pos = s->err = 0;
    *size = 0;
    if (!strcmp(name, "-")) {
        s->fd = STDIN_FILENO;
        return 0;
    }
    s->fd = sys_open(name, O_RDONLY);
    if (s->fd < 0) {
        dprintf(STDERR_FILENO, "cmp: %s: %s\n", name, strerror(s->fd));
        return -1;
    }
    dirent_t d;
    if (sys_stat(name, &d) >= 0) *size = d.size;
    return 0;
}

int main(int argc, char** argv) {
    int list = 0, silent = 0, a = 1;
    uint32_t limit = 0xFFFFFFFFu;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "--")) {
            a++;
            break;
        }
        for (const char* o = argv[a] + 1; *o; o++) {
            if (*o == 'l') list = 1;
            else if (*o == 's') silent = 1;
            else if (*o == 'n') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (parse_uint(v, &limit)) {
                    dprintf(STDERR_FILENO, "cmp: invalid --bytes value '%s'\n", v);
                    return 2;
                }
                break;
            } else {
                dprintf(STDERR_FILENO, "cmp: invalid option -- '%c'\n", *o);
                return usage();
            }
        }
    }
    int n = argc - a;
    if (n < 1) {
        dprintf(STDERR_FILENO, "cmp: missing operand\n");
        return usage();
    }
    if (n > 2) {
        dprintf(STDERR_FILENO, "cmp: extra operand '%s'\n", argv[a + 2]);
        return usage();
    }
    static src_t f1, f2;
    uint32_t size1, size2;
    if (open_src(&f1, argv[a], &size1)) return 2;
    if (open_src(&f2, n == 2 ? argv[a + 1] : "-", &size2)) return 2;

    // for -l, the byte numbers are as wide as the largest possible one
    int width = 1;
    for (uint32_t m = size1 < size2 ? size1 : size2; m >= 10; m /= 10) width++;

    lio_out_t out;
    lio_out_init(&out, STDOUT_FILENO, "cmp");
    uint32_t pos = 0, line = 1;
    int status = 0;
    while (pos < limit) {
        int c1 = next(&f1), c2 = next(&f2);
        if (c1 < 0 || c2 < 0) {
            if (f1.err || f2.err) {
                lio_flush(&out);
                src_t* e = f1.err ? &f1 : &f2;
                dprintf(STDERR_FILENO, "cmp: %s: %s\n", e->name, strerror(e->err));
                return 2;
            }
            if (c1 != c2) {  // one is shorter
                lio_flush(&out);
                if (!silent) dprintf(STDERR_FILENO, "cmp: EOF on %s after byte %u\n", c1 < 0 ? f1.name : f2.name, pos);
                status = 1;
            }
            break;
        }
        pos++;
        if (c1 != c2) {
            status = 1;
            if (silent) break;
            char b[80];
            if (!list) {
                int len = snprintf(b, sizeof b, "%s %s differ: byte %u, line %u\n", f1.name, f2.name, pos, line);
                lio_put(&out, b, (uint32_t)len);
                break;
            }
            int len = snprintf(b, sizeof b, "%*u ", width, pos);
            for (int k = 0; k < 2; k++) {
                int c = k ? c2 : c1;
                // octal, at least 3 wide, blank padded
                int hi = c >> 6, mid = (c >> 3) & 7, lo = c & 7;
                b[len++] = hi ? (char)('0' + hi) : ' ';
                b[len++] = hi || mid ? (char)('0' + mid) : ' ';
                b[len++] = (char)('0' + lo);
                b[len++] = k ? '\n' : ' ';
            }
            lio_put(&out, b, (uint32_t)len);
        }
        if (c1 == '\n') line++;
    }
    lio_flush(&out);
    return status;
}

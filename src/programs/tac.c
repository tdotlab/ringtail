// tac [-b] [-s SEPARATOR] [FILE...]
//
// Writes each FILE (or standard input, or "-") with its records in
// reverse order: the last one first. A record ends with the separator,
// a newline unless -s gives another string, and the separator stays at
// the end of its record. Each file is turned round by itself.
//   -b  the separator comes before its record instead of after
//   -s  use this string as the separator; an empty string means a NUL byte
// A last record with no separator on it is written first, with none, so
// `printf 'a\nb' | tac` prints "ba" and a newline. Where a separator of
// more than one byte could be found in more than one way (XX in XXX)
// the match nearest the end of the file wins, as in GNU tac. There is no
// -r: the separator is always a plain string.
//
// tac must have all of a file before it can write anything, so it reads
// it into memory first. On the screen, a last line with no newline gets
// one so the prompt starts clean.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: tac [-b] [-s SEPARATOR] [FILE...]\n");
    return 2;
}

static const char* sep = "\n";
static uint32_t seplen = 1;
static int before;
static lio_out_t out;
static int last = '\n';  // the last byte written

static void put(const char* p, uint32_t n) {
    if (!n) return;
    last = (unsigned char)p[n - 1];
    lio_put(&out, p, n);
}

// All of fd, in a malloc'd buffer. 0, or a negative errno.
static int read_all(int fd, char** buf, uint32_t* len) {
    uint32_t cap = 4096, n = 0;
    char* b = malloc(cap);
    if (!b) return -ENOMEM;
    for (;;) {
        if (n == cap) {
            if (cap >= 0x20000000u) {
                free(b);
                return -ENOMEM;
            }
            char* g = realloc(b, cap * 2);
            if (!g) {
                free(b);
                return -ENOMEM;
            }
            b = g;
            cap *= 2;
        }
        int r = sys_read(fd, b + n, cap - n);
        if (r < 0) {
            free(b);
            return r;
        }
        if (r == 0) break;
        n += (uint32_t)r;
    }
    *buf = b;
    *len = n;
    return 0;
}

// b[0..n) with its records last first. The separators are looked for
// from the end backwards, none overlapping the one found before it;
// hi is the end of what has not been written yet.
static void reverse_records(const char* b, uint32_t n) {
    uint32_t hi = n;
    if (!before) {
        // A record ends where a separator ends.
        for (uint32_t e = n; e >= seplen;) {
            if (!memcmp(b + e - seplen, sep, seplen)) {
                put(b + e, hi - e);
                hi = e;
                e -= seplen;
            } else {
                e--;
            }
        }
    } else if (n >= seplen) {
        // A record starts where a separator starts.
        for (uint32_t s = n - seplen;;) {
            if (!memcmp(b + s, sep, seplen)) {
                put(b + s, hi - s);
                hi = s;
                if (s < seplen) break;
                s -= seplen;
            } else {
                if (!s) break;
                s--;
            }
        }
    }
    put(b, hi);
}

int main(int argc, char** argv) {
    int nops = 0, only_ops = 0;
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
        for (const char* o = s + 1; *o; o++) {
            if (*o == 'b') {
                before = 1;
            } else if (*o == 's') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                if (*v) {
                    sep = v;
                    seplen = (uint32_t)strlen(v);
                } else {
                    sep = "";  // its one NUL byte is the separator
                    seplen = 1;
                }
                break;
            } else {
                return usage();
            }
        }
    }
    if (!nops && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    lio_out_init(&out, STDOUT_FILENO, "tac");
    int bad = 0;
    for (int i = 0; i < nops || i == 0; i++) {
        int stdin_arg = !nops || !strcmp(argv[1 + i], "-");
        const char* name = stdin_arg ? "standard input" : argv[1 + i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (fd < 0) {
            fs_error("tac", name, fd);
            bad = 1;
            continue;
        }
        char* buf = 0;
        uint32_t len = 0;
        int e = read_all(fd, &buf, &len);
        if (!stdin_arg) sys_close(fd);
        if (e) {
            dprintf(STDERR_FILENO, "tac: %s: %s\n", name, e == -ENOMEM ? "out of memory" : strerror(e));
            bad = 1;
            continue;
        }
        reverse_records(buf, len);
        free(buf);
    }
    if (last != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) lio_putc(&out, '\n');
    lio_flush(&out);
    return bad;
}

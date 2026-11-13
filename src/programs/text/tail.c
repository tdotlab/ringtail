// tail [-n N | -c N] [-q] [FILE...]    print the end of each file
//
// The last 10 lines of each FILE, or of standard input when none is
// named (or for "-"). -n N takes the last N lines, -c N the last N
// bytes; -N is short for -n N. A count with a plus, as in -n +3, means
// everything from line 3 on (or from byte 3 with -c). With more than
// one file each one gets a "==> name <==" heading, which -q leaves
// out. There is no -f: nothing here could say when a file has grown.
//
// A file is read through to its end whichever way it is asked for, so
// tail never holds more than the lines or bytes it was asked to keep
// (plus up to 64 KB of slack before it trims).
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: tail [-n [+]N | -c [+]N] [-q] [FILE...]\n");
    return 2;
}

#define SLACK 65536u         // keep at least this much before trimming

static int bytes_mode;       // -c: count bytes, not lines
static int from_start;       // +N: everything from the Nth on
static uint32_t count = 10;

static void put(const char* p, uint32_t n) {
    while (n) {
        int w = sys_write(STDOUT_FILENO, p, n);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "tail: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        n -= (uint32_t)w;
    }
}

// +N: skip the first N-1 lines (or bytes), print the rest as it comes.
static int from_fd(int fd) {
    char buf[1024];
    uint32_t skip = count ? count - 1 : 0;  // +0 is the same as +1
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        uint32_t i = 0, len = (uint32_t)n;
        if (skip && bytes_mode) {
            i = skip < len ? skip : len;
            skip -= i;
        } else {
            while (i < len && skip)
                if (buf[i++] == '\n') skip--;
        }
        if (i < len) put(buf + i, len - i);
    }
    return n;
}

static char* keep;           // the end of what has been read so far
static uint32_t klen, kcap;

// Where the last `count` lines (or bytes) of keep[] begin.
static uint32_t tail_start(void) {
    if (bytes_mode) return klen > count ? klen - count : 0;
    if (count == 0) return klen;
    uint32_t i = klen, seen = 0;
    if (i && keep[i - 1] == '\n') i--;  // the last line's own newline ends no earlier line
    while (i > 0)
        if (keep[--i] == '\n' && ++seen == count) return i + 1;
    return 0;
}

static void trim(void) {
    uint32_t s = tail_start();
    if (s) {
        memmove(keep, keep + s, klen - s);
        klen -= s;
    }
}

// -N: the last N, wherever they start.
static int last_fd(int fd) {
    char buf[1024];
    uint32_t trim_at = SLACK;
    int n;
    klen = 0;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        if (klen + (uint32_t)n > kcap) {
            uint32_t want = kcap ? kcap * 2 : 2 * SLACK;
            while (want < klen + (uint32_t)n) want *= 2;
            char* p = realloc(keep, want);
            if (!p) {
                dprintf(STDERR_FILENO, "tail: out of memory\n");
                sys_exit(1);
            }
            keep = p;
            kcap = want;
        }
        memcpy(keep + klen, buf, (size_t)n);
        klen += (uint32_t)n;
        if (klen > trim_at) {
            trim();
            trim_at = klen * 2 > SLACK ? klen * 2 : SLACK;
        }
    }
    if (n < 0) return n;
    uint32_t s = tail_start();
    put(keep + s, klen - s);
    return 0;
}

static int tail_fd(int fd, const char* name) {
    int r = from_start ? from_fd(fd) : last_fd(fd);
    if (r < 0) {
        dprintf(STDERR_FILENO, "tail: %s: %s\n", name, strerror(r));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int quiet = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char* o = argv[a] + 1;
        const char* v = 0;
        if (!strcmp(argv[a], "--")) { a++; break; }
        if (*o >= '0' && *o <= '9') {  // -5
            v = o;
            bytes_mode = 0;
        } else if (!strcmp(o, "q")) {
            quiet = 1;
            continue;
        } else if (*o == 'n' || *o == 'c') {  // -n 5, -n5, -n +5
            v = o + 1;
            if (!*v) {
                if (++a == argc) return usage();
                v = argv[a];
            }
            bytes_mode = *o == 'c';
        } else {
            return usage();
        }
        const char* digits = v;
        from_start = *v == '+';
        if (from_start) digits++;
        if (parse_uint(digits, &count)) {
            dprintf(STDERR_FILENO, "tail: invalid number of %s: '%s'\n", bytes_mode ? "bytes" : "lines", v);
            return 2;
        }
    }

    int files = argc - a;
    if (files == 0) return tail_fd(STDIN_FILENO, "standard input");
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int stdin_arg = !strcmp(argv[i], "-");
        int fd = stdin_arg ? STDIN_FILENO : sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "tail: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        if (files > 1 && !quiet) {
            char h[PATH_MAX + 16];
            int len = snprintf(h, sizeof h, "%s==> %s <==\n", i > a ? "\n" : "", stdin_arg ? "standard input" : argv[i]);
            put(h, (uint32_t)len);
        }
        bad |= tail_fd(fd, stdin_arg ? "standard input" : argv[i]);
        if (!stdin_arg) sys_close(fd);
    }
    return bad;
}

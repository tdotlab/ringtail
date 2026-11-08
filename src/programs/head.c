// head [-n N | -c N] [-q] [FILE...]    print the start of each file
//
// The first 10 lines of each FILE, or of standard input when none is
// named (or for "-"). -n N takes N lines instead, -c N the first N
// bytes; -N is short for -n N. With more than one file each one gets
// a "==> name <==" heading, which -q leaves out. head stops reading as
// soon as it has enough, so `cat big | head` does not wait for big.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: head [-n N | -c N] [-q] [FILE...]\n");
    return 2;
}

static int bytes_mode;       // -c: count bytes, not lines
static uint32_t limit = 10;

// All of it, or give up: a reader that has gone away ends head quietly.
static void put(const char* p, uint32_t n) {
    while (n) {
        int w = sys_write(STDOUT_FILENO, p, n);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "head: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        n -= (uint32_t)w;
    }
}

static int head_fd(int fd, const char* name) {
    char buf[1024];
    uint32_t left = limit;
    int n = 0;
    while (left && (n = sys_read(fd, buf, sizeof buf)) > 0) {
        uint32_t take = (uint32_t)n;
        if (bytes_mode) {
            if (take > left) take = left;
            left -= take;
        } else {
            take = 0;
            while (take < (uint32_t)n) {
                char c = buf[take++];
                if (c == '\n' && --left == 0) break;
            }
        }
        put(buf, take);
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "head: %s: %s\n", name, strerror(n));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int quiet = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char* o = argv[a] + 1;
        if (!strcmp(argv[a], "--")) { a++; break; }
        if (*o >= '0' && *o <= '9') {  // -5
            if (parse_uint(o, &limit)) return usage();
            bytes_mode = 0;
        } else if (!strcmp(o, "q")) {
            quiet = 1;
        } else if (*o == 'n' || *o == 'c') {  // -n 5, -n5
            const char* v = o + 1;
            if (!*v) {
                if (++a == argc) return usage();
                v = argv[a];
            }
            if (parse_uint(v, &limit)) {
                dprintf(STDERR_FILENO, "head: invalid number of %s: '%s'\n", *o == 'n' ? "lines" : "bytes", v);
                return 2;
            }
            bytes_mode = *o == 'c';
        } else {
            return usage();
        }
    }

    int files = argc - a;
    if (files == 0) return head_fd(STDIN_FILENO, "standard input");
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int stdin_arg = !strcmp(argv[i], "-");
        int fd = stdin_arg ? STDIN_FILENO : sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "head: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        if (files > 1 && !quiet) {
            char h[PATH_MAX + 16];
            int len = snprintf(h, sizeof h, "%s==> %s <==\n", i > a ? "\n" : "", stdin_arg ? "standard input" : argv[i]);
            put(h, (uint32_t)len);
        }
        bad |= head_fd(fd, stdin_arg ? "standard input" : argv[i]);
        if (!stdin_arg) sys_close(fd);
    }
    return bad;
}

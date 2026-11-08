// cat FILE...    write files, one after another
//
// Each FILE in turn, or standard input for "-". With no FILE, standard
// input is what is read, unless that is the keyboard, which would only
// wait: then cat shows its usage. A file that cannot be read is
// reported and the rest still follow; the status is 1 if any failed.
//
// On the screen, a last line with no newline gets one so the prompt
// starts clean. Into a pipe or a file the bytes pass on exactly, so
// `cat a b > c` is a join.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: cat FILE...\n");
    return 2;
}

static int last = '\n';

// All of it, or give up: a reader that has gone away ends cat quietly.
static void put(const char* p, uint32_t n) {
    while (n) {
        int w = sys_write(STDOUT_FILENO, p, n);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "cat: write error: %s\n", w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        n -= (uint32_t)w;
    }
}

static int copy(int fd, const char* name) {
    char buf[1024];
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        put(buf, (uint32_t)n);
        last = buf[n - 1];
    }
    if (n < 0) {
        fs_error("cat", name, n);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    for (int i = a; i < argc; i++)
        if (!dd && argv[i][0] == '-' && argv[i][1]) return usage();
    if (a == argc && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    int bad = 0;
    for (int i = a; i < argc || i == a; i++) {
        int stdin_arg = a == argc || !strcmp(argv[i], "-");
        const char* name = stdin_arg ? "standard input" : argv[i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            fs_error("cat", name, fd);
            bad = 1;
            continue;
        }
        bad |= copy(fd, name);
        if (!stdin_arg) sys_close(fd);
    }
    if (last != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) put("\n", 1);
    return bad;
}

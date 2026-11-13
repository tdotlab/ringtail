// cat [-n] [FILE...]    write files, one after another
//
// Each FILE in turn, or standard input for "-". With no FILE, standard
// input is what is read, unless that is the keyboard, which would only
// wait: then cat shows its usage. A file that cannot be read is
// reported and the rest still follow; the status is 1 if any failed.
// -n puts a line number in front of every line, counting on from one
// file to the next.
//
// On the screen, a last line with no newline gets one so the prompt
// starts clean. Into a pipe or a file the bytes pass on exactly, so
// `cat a b > c` is a join.
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: cat [-n] [FILE...]\n");
    return 2;
}

static int last = '\n';
static int numbered;
static uint32_t line_no;
static int at_start = 1;

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

// buf with a number before each line. Out takes up to eight bytes more
// than it was given for every newline, so it is sized for that.
static void put_numbered(const char* buf, int n) {
    char out[1024 * 9];
    uint32_t o = 0;
    for (int i = 0; i < n; i++) {
        if (at_start) {
            o += (uint32_t)snprintf(out + o, 16, "%6u\t", ++line_no);
            at_start = 0;
        }
        out[o++] = buf[i];
        if (buf[i] == '\n') at_start = 1;
    }
    put(out, o);
}

static int copy(int fd, const char* name) {
    char buf[1024];
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        if (numbered) put_numbered(buf, n);
        else put(buf, (uint32_t)n);
        last = buf[n - 1];
    }
    if (n < 0) {
        fs_error("cat", name, n);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "n", got)) return usage();
    numbered = has_flag(got, 'n');
    if (argc == 1 && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    int bad = 0;
    for (int i = 1; i < argc || i == 1; i++) {
        int stdin_arg = argc == 1 || !strcmp(argv[i], "-");
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

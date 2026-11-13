// tee [-a] [-i] [FILE...]
//
// Copies standard input to standard output and to every FILE. -a appends
// to the files instead of replacing them; -i is accepted and ignored. A
// file named "-" is an ordinary file, as in GNU tee. A file that cannot
// be opened is reported and skipped, and tee exits 1 at the end; the rest
// still get the data. Each chunk read is written out before tee waits for
// the next, so it works in the middle of a slow pipeline.
#include "ulib.h"

#define MAX_FILES 16

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: tee [-ai] [FILE...]\n");
    return 2;
}

// All of buf to fd. 0, or the failing result of the write.
static int write_all(int fd, const char* p, uint32_t n) {
    while (n) {
        int w = sys_write(fd, p, n);
        if (w <= 0) return w ? w : -EIO;
        p += w;
        n -= (uint32_t)w;
    }
    return 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "ai", got)) return usage();
    int append = has_flag(got, 'a');
    if (argc - 1 > MAX_FILES) {
        dprintf(STDERR_FILENO, "tee: at most %d files\n", MAX_FILES);
        return 2;
    }

    int fds[MAX_FILES + 1];
    const char* names[MAX_FILES + 1];
    int n = 1, bad = 0;
    fds[0] = STDOUT_FILENO;
    names[0] = "standard output";
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            dprintf(STDERR_FILENO, "tee: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        fds[n] = fd;
        names[n++] = argv[i];
    }

    char buf[1024];
    int r;
    while ((r = sys_read(STDIN_FILENO, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            if (fds[i] < 0) continue;  // gave up on this one
            int e = write_all(fds[i], buf, (uint32_t)r);
            if (!e) continue;
            if (i == 0 && e == -EPIPE) sys_exit(bad);  // the reader left: nothing more to do
            dprintf(STDERR_FILENO, "tee: %s: %s\n", names[i], strerror(e));
            bad = 1;
            if (i) {
                sys_close(fds[i]);
                fds[i] = -1;
            } else {
                fds[0] = -1;
            }
        }
    }
    if (r < 0) {
        dprintf(STDERR_FILENO, "tee: standard input: %s\n", strerror(r));
        bad = 1;
    }
    for (int i = 1; i < n; i++)
        if (fds[i] >= 0) sys_close(fds[i]);
    return bad;
}

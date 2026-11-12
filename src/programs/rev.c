// rev [FILE...]
//
// Writes each line of each FILE (or standard input, or "-") back to
// front. It works on bytes, not on characters: the text on the screen
// and on the disk is one byte a letter, and a UTF-8 letter would come
// out in pieces. A last line with no newline stays without one (but on
// the screen it gets one, so the prompt starts clean); nothing is put
// between one file and the next. A file that cannot be read is reported
// and the rest still follow; the status is 1 if any failed.
//
// Lines are written as they are decided, so at the end of a slow
// pipeline they show up one at a time.
#include "ulib.h"
#include "lineio.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: rev [FILE...]\n");
    return 2;
}

static lio_out_t out;
static int last = '\n';

static int rev_fd(int fd, const char* name) {
    lio_in_t in;
    lio_in_init(&in, fd);
    lio_line_t l = { 0, 0, 0, 0 };
    while (lio_getline(&in, &out, &l)) {
        for (uint32_t i = 0, j = l.n; i + 1 < j; i++, j--) {
            char t = l.s[i];
            l.s[i] = l.s[j - 1];
            l.s[j - 1] = t;
        }
        lio_put(&out, l.s, l.n);
        if (l.had_nl) lio_putc(&out, '\n');
        if (l.had_nl) last = '\n';
        else if (l.n) last = (unsigned char)l.s[l.n - 1];  // the last byte written
    }
    free(l.s);
    if (in.err) {
        dprintf(STDERR_FILENO, "rev: %s: %s\n", name, strerror(in.err));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "", got)) return usage();
    if (argc == 1 && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY) return usage();

    lio_out_init(&out, STDOUT_FILENO, "rev");
    int bad = 0;
    for (int i = 1; i < argc || i == 1; i++) {
        int stdin_arg = argc == 1 || !strcmp(argv[i], "-");
        const char* name = stdin_arg ? "standard input" : argv[i];
        int fd = stdin_arg ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (fd < 0) {
            fs_error("rev", name, fd);
            bad = 1;
            continue;
        }
        bad |= rev_fd(fd, name);
        if (!stdin_arg) sys_close(fd);
    }
    if (last != '\n' && sys_fdtype(STDOUT_FILENO) == FDTYPE_TTY) lio_putc(&out, '\n');
    lio_flush(&out);
    return bad;
}

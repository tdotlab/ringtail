// yes [STRING...]    print STRING (default "y") over and over
//
// The arguments are joined with spaces and printed as a line, again and
// again, until the reader goes away or Ctrl+C: `yes | head -n 3`.
#include "ulib.h"

int main(int argc, char** argv) {
    char line[256];
    uint32_t n = 0;
    if (argc < 2) {
        line[n++] = 'y';
    } else {
        for (int i = 1; i < argc; i++) {
            uint32_t len = (uint32_t)strlen(argv[i]);
            if (n + len + 2 > sizeof line) {
                dprintf(STDERR_FILENO, "yes: the text is too long\n");
                return 1;
            }
            if (i > 1) line[n++] = ' ';
            memcpy(line + n, argv[i], len);
            n += len;
        }
    }
    line[n++] = '\n';

    // Fill a buffer with whole lines, then send it again and again.
    static char buf[1024];
    uint32_t used = 0;
    while (used + n <= sizeof buf) {
        memcpy(buf + used, line, n);
        used += n;
    }
    for (;;) {
        const char* p = buf;
        uint32_t left = used;
        while (left) {
            int w = sys_write(STDOUT_FILENO, p, left);
            if (w <= 0) {
                if (w != -EPIPE) dprintf(STDERR_FILENO, "yes: write error: %s\n", w ? strerror(w) : "no progress");
                return 1;
            }
            p += w;
            left -= (uint32_t)w;
        }
    }
}

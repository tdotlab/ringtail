// wc [FILE...]: count lines, words and bytes, of standard input when
// no file is named.
#include "ulib.h"

static int count(int fd, const char* name, uint32_t total[3]) {
    char buf[512];
    uint32_t lines = 0, words = 0, bytes = 0;
    int in_word = 0, n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        bytes += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            int space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (!space && !in_word) words++;
            in_word = !space;
        }
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "wc: %s: %s\n", name ? name : "input", strerror(n));
        return 1;
    }
    printf("%7u %7u %7u%s%s\n", lines, words, bytes, name ? " " : "", name ? name : "");
    total[0] += lines; total[1] += words; total[2] += bytes;
    return 0;
}

int main(int argc, char** argv) {
    uint32_t total[3] = { 0, 0, 0 };
    if (argc < 2) return count(STDIN_FILENO, 0, total);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        bad |= count(fd, argv[i], total);
        sys_close(fd);
    }
    if (argc > 2) printf("%7u %7u %7u total\n", total[0], total[1], total[2]);
    return bad;
}

// wc [-lwcmL] [FILE...]    count lines, words and bytes
//
// Of standard input when no file is named (or for "-"). -l counts
// lines, -w words, -c bytes, -m characters (the same thing here: the
// console is 8-bit), -L the length of the longest line (a tab runs to
// the next multiple of 8). With none of them wc prints lines, words and
// bytes; with some, only those, always in that order. A word is a run
// of anything but white space (space, tab, newline, \v, \f, \r). With
// more than one number or file the columns are 7 wide, so they line up
// down a listing; one number for one file is printed bare, as $(wc -l
// < f) wants it. More than one file gets a total at the end.
#include "ulib.h"

enum { C_LINES, C_WORDS, C_BYTES, C_LONGEST, NCOLS };

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: wc [-lwcmL] [FILE...]\n");
    return 2;
}

static int want[NCOLS];
static int ncols;

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static void show(const uint32_t v[NCOLS], const char* name, int bare) {
    int first = 1;
    for (int c = 0; c < NCOLS; c++) {
        if (!want[c]) continue;
        if (bare) printf("%u", v[c]);
        else printf(first ? "%7u" : " %7u", v[c]);
        first = 0;
    }
    if (name) printf(" %s", name);
    print("\n");
}

// Counts one stream. Returns 0, or 1 after a message on a read error.
static int count(int fd, const char* name, uint32_t sum[NCOLS], int bare) {
    char buf[512];
    uint32_t v[NCOLS] = { 0, 0, 0, 0 };
    uint32_t pos = 0;  // column in the current line
    int in_word = 0, n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        v[C_BYTES] += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            int space = is_space(c);
            if (!space && !in_word) v[C_WORDS]++;
            in_word = !space;
            if (c == '\n' || c == '\r' || c == '\f') {
                if (c == '\n') v[C_LINES]++;
                if (pos > v[C_LONGEST]) v[C_LONGEST] = pos;
                pos = 0;
            } else if (c == '\t') {
                pos = (pos | 7) + 1;
            } else if (c >= ' ' && c <= '~') {
                pos++;
            }
        }
    }
    if (pos > v[C_LONGEST]) v[C_LONGEST] = pos;
    if (n < 0) {
        dprintf(STDERR_FILENO, "wc: %s: %s\n", name ? name : "standard input", strerror(n));
        return 1;
    }
    show(v, name, bare);
    for (int c = 0; c < NCOLS; c++) {
        if (c == C_LONGEST) {
            if (v[c] > sum[c]) sum[c] = v[c];  // the longest of the lot
        } else {
            sum[c] += v[c];
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "lwcmL", got)) return usage();
    want[C_LINES] = has_flag(got, 'l');
    want[C_WORDS] = has_flag(got, 'w');
    want[C_BYTES] = has_flag(got, 'c') || has_flag(got, 'm');
    want[C_LONGEST] = has_flag(got, 'L');
    if (!got[0]) want[C_LINES] = want[C_WORDS] = want[C_BYTES] = 1;
    for (int c = 0; c < NCOLS; c++) ncols += want[c];

    uint32_t sum[NCOLS] = { 0, 0, 0, 0 };
    int bare = ncols == 1 && argc <= 2;
    if (argc < 2) return count(STDIN_FILENO, 0, sum, bare);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            bad |= count(STDIN_FILENO, "-", sum, bare);
            continue;
        }
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        bad |= count(fd, argv[i], sum, bare);
        sys_close(fd);
    }
    if (argc > 2) show(sum, "total", 0);
    return bad;
}

// grep [-i] TEXT [FILE...]: print the lines that contain TEXT, from
// standard input when no file is named. -i ignores case. Plain text,
// not patterns. Exits 0 if a line matched, 1 if none, 2 on an error.
#include "ulib.h"

#define LINE_MAX 1024

static int fold;

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int contains(const char* line, uint32_t len, const char* text, uint32_t tlen) {
    if (tlen > len) return 0;
    for (uint32_t i = 0; i + tlen <= len; i++) {
        uint32_t k = 0;
        while (k < tlen && (fold ? lower(line[i + k]) == lower(text[k]) : line[i + k] == text[k])) k++;
        if (k == tlen) return 1;
    }
    return 0;
}

typedef struct { const char* name; const char* text; uint32_t tlen; int show_name, matched; } job_t;

static void emit(job_t* j, const char* line, uint32_t len) {
    if (!contains(line, len, j->text, j->tlen)) return;
    j->matched = 1;
    if (j->show_name) printf("%s:", j->name);
    sys_write(STDOUT_FILENO, line, len);
    print("\n");
}

// Lines longer than LINE_MAX are matched and printed in pieces.
static int scan(int fd, job_t* j) {
    static char line[LINE_MAX];
    char buf[512];
    uint32_t len = 0;
    int n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                line[len++] = buf[i];
                if (len < LINE_MAX) continue;
            }
            emit(j, line, len);
            len = 0;
        }
    }
    if (len) emit(j, line, len);  // a last line with no newline
    if (n < 0) {
        dprintf(STDERR_FILENO, "grep: %s: %s\n", j->name, strerror(n));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int a = 1;
    if (a < argc && strcmp(argv[a], "-i") == 0) { fold = 1; a++; }
    if (a >= argc) {
        dprintf(STDERR_FILENO, "usage: grep [-i] TEXT [FILE...]\n");
        return 2;
    }
    job_t j = { "input", argv[a], (uint32_t)strlen(argv[a]), argc - a > 2, 0 };
    a++;
    int bad = 0;
    if (a == argc) bad = scan(STDIN_FILENO, &j);
    for (int i = a; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "grep: %s: %s\n", argv[i], strerror(fd));
            bad = 1;
            continue;
        }
        j.name = argv[i];
        bad |= scan(fd, &j);
        sys_close(fd);
    }
    return bad ? 2 : j.matched ? 0 : 1;
}

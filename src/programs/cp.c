// cp SRC DST, or cp SRC... DIR: copy files. An existing DST file is
// replaced; directories are not copied.
#include "ulib.h"

static char buf[4096];

static int fail(const char* what, const char* why) {
    dprintf(STDERR_FILENO, "cp: %s: %s\n", what, why);
    return 1;
}

static int copy(const char* src, const char* dst) {
    char to[PATH_MAX];
    dirent_t s, d;
    int r = sys_stat(src, &s);
    if (r < 0) return fail(src, strerror(r));
    if (s.flags & DIRENT_DIR) return fail(src, "is a directory (not copied)");
    r = dest_path(src, dst, to, sizeof to);
    if (r < 0) return fail(dst, strerror(r));
    int existed = sys_stat(to, &d) == 0;
    if (existed && (d.flags & DIRENT_DIR)) return fail(to, "is a directory");
    if (existed && d.id == s.id) return fail(to, "is the same file");

    int in = sys_open(src, O_RDONLY);
    if (in < 0) return fail(src, strerror(in));
    int out = sys_open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { sys_close(in); return fail(to, strerror(out)); }
    int n, bad = 0;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        int w = sys_write(out, buf, (uint32_t)n);
        if (w != n) { bad = fail(to, strerror(w < 0 ? w : -ENOSPC)); break; }
    }
    if (n < 0) bad = fail(src, strerror(n));
    sys_close(in);
    sys_close(out);
    if (bad && !existed) sys_delete(to);  // no half copies left behind
    return bad;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: cp SRC DST, or cp SRC... DIR\n");
        return 2;
    }
    const char* dst = argv[argc - 1];
    if (argc > 3) {
        dirent_t d;
        if (sys_stat(dst, &d) < 0 || !(d.flags & DIRENT_DIR)) {
            dprintf(STDERR_FILENO, "cp: copying several files needs a directory last\n");
            return 2;
        }
    }
    int bad = 0;
    for (int i = 1; i < argc - 1; i++) bad |= copy(argv[i], dst);
    return bad;
}

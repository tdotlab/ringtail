// mkdir [-p] DIR...    make directories
//
// -p also makes the parents that are missing, and is not bothered by a
// directory that is already there. Without it, an existing name is an
// error.
#include "ulib.h"

// Each part of path in turn, so "a/b/c" makes a, a/b, then a/b/c.
static int make_path(const char* path) {
    char p[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof p) return -ENAMETOOLONG;
    memcpy(p, path, len + 1);
    while (len > 1 && p[len - 1] == '/') p[--len] = 0;  // a trailing slash
    for (size_t i = 1; i <= len; i++) {
        if (p[i] != '/' && p[i]) continue;
        char keep = p[i];
        p[i] = 0;
        dirent_t d;
        int r = sys_stat(p, &d);
        if (r == 0 && !(d.flags & DIRENT_DIR)) return -ENOTDIR;
        if (r == -ENOENT) r = sys_mkdir(p);
        if (r < 0) return r;
        p[i] = keep;
    }
    return 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "p", got) || argc < 2) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] DIR...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int r = has_flag(got, 'p') ? make_path(argv[i]) : sys_mkdir(argv[i]);
        if (r < 0) {
            fs_error("mkdir", argv[i], r);
            bad = 1;
        }
    }
    return bad;
}

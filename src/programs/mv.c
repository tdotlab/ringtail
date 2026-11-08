// mv SRC DST, or mv SRC... DIR    move or rename
//
// A file or a whole directory. If DST is an existing directory the
// thing goes inside it, under its own name. Something already at the
// target is never replaced.
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (argc - a < 2 || (!dd && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: mv SRC DST, or mv SRC... DIR\n");
        return 2;
    }
    const char* dst = argv[argc - 1];
    if (argc - a > 2) {
        dirent_t d;
        if (sys_stat(dst, &d) < 0 || !(d.flags & DIRENT_DIR)) {
            dprintf(STDERR_FILENO, "mv: moving several things needs a directory last\n");
            return 2;
        }
    }
    int bad = 0;
    for (int i = a; i < argc - 1; i++) {
        const char* src = argv[i];
        char to[PATH_MAX];
        int r = dest_path(src, dst, to, sizeof to);
        if (!r) r = sys_rename(src, to);
        if (r == -EINVAL && !name_ok(to)) fs_error("mv", to, r);
        else if (r == -EINVAL) dprintf(STDERR_FILENO, "mv: %s: cannot go inside itself\n", src);
        else if (r == -EBUSY) dprintf(STDERR_FILENO, "mv: %s: give it by name to move it\n", src);
        else if (r == -EEXIST) dprintf(STDERR_FILENO, "mv: %s: already exists (rm it first)\n", to);
        else if (r < 0) fs_error("mv", src, r);
        bad |= r < 0;
    }
    return bad;
}

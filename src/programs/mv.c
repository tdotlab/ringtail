// mv [-f | -n] SRC DST, or mv [-f | -n] SRC... DIR    move or rename
//
// A file or a whole directory. If DST is an existing directory the
// thing goes inside it, under its own name. A file already at the target
// is replaced (-f says so; -n leaves it alone and says nothing). A
// directory is never replaced, nor a file by a directory. A name that
// differs only in case is a rename (the disk ignores case).
//
// The old target goes first and the move second, so a move that then
// fails has still lost the old file: every check that can be made is
// made before anything is deleted.
#include "ulib.h"

static int move_one(const char* src, const char* dst, int keep) {
    char to[PATH_MAX];
    dirent_t s, t;
    int r = sys_stat(src, &s);
    if (r < 0) {
        fs_error("mv", src, r);
        return 1;
    }
    r = dest_path(src, dst, to, sizeof to);
    if (r < 0) {
        fs_error("mv", dst, r);
        return 1;
    }
    // The disk ignores case, so a name that differs only in case finds
    // the thing itself: that is a rename, not a clash.
    int recase = sys_stat(to, &t) == 0 && t.id == s.id && strcmp(src, to) && !strcasecmp(src, to);
    if (!recase && sys_stat(to, &t) == 0) {
        if (t.id == s.id) {
            dprintf(STDERR_FILENO, "mv: %s: and %s are the same file\n", src, to);
            return 1;
        }
        if (keep) return 0;
        if ((t.flags & DIRENT_DIR) || (s.flags & DIRENT_DIR)) {
            dprintf(STDERR_FILENO, "mv: %s: already exists (and a directory is never replaced)\n", to);
            return 1;
        }
        if (!name_ok(to)) {
            fs_error("mv", to, -EINVAL);
            return 1;
        }
        r = sys_delete(to);
        if (r < 0) {
            fs_error("mv", to, r);
            return 1;
        }
    }
    r = sys_rename(src, to);
    if (r == -EINVAL && !name_ok(to)) fs_error("mv", to, r);
    else if (r == -EINVAL) dprintf(STDERR_FILENO, "mv: %s: cannot go inside itself\n", src);
    else if (r == -EBUSY) dprintf(STDERR_FILENO, "mv: %s: give it by name to move it\n", src);
    else if (r < 0) fs_error("mv", src, r);
    return r < 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "fn", got) || argc < 3 || (has_flag(got, 'f') && has_flag(got, 'n'))) {
        dprintf(STDERR_FILENO, "usage: mv [-f | -n] SRC DST, or mv [-f | -n] SRC... DIR\n");
        return 2;
    }
    const char* dst = argv[argc - 1];
    if (argc > 3) {
        dirent_t d;
        if (sys_stat(dst, &d) < 0 || !(d.flags & DIRENT_DIR)) {
            dprintf(STDERR_FILENO, "mv: moving several things needs a directory last\n");
            return 2;
        }
    }
    int bad = 0;
    for (int i = 1; i < argc - 1; i++) bad |= move_one(argv[i], dst, has_flag(got, 'n'));
    return bad;
}

// rm [-f] [-r] FILE...    delete files, or whole trees
//
// -r (or -R) also removes directories, with everything inside them.
// -f says nothing about a FILE that is not there, and takes no operands
// at all without complaint. "." and ".." and "/" are never removed.
// Without -r a directory is refused.
#include "ulib.h"

static int force, recurse;

// "", ".", ".." or "/" as the last part of path, however many slashes.
static int is_dot(const char* path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n == 1 && path[0] == '/') return 1;
    size_t at = n;
    while (at > 0 && path[at - 1] != '/') at--;
    size_t len = n - at;
    return (len == 1 && path[at] == '.') || (len == 2 && path[at] == '.' && path[at + 1] == '.');
}

static int remove_one(const char* path) {
    dirent_t d;
    int r = sys_stat(path, &d);
    if (r < 0) {
        if (r == -ENOENT && force) return 0;
        fs_error("rm", path, r);
        return 1;
    }
    if (!(d.flags & DIRENT_DIR)) {
        r = sys_delete(path);
        if (r < 0) fs_error("rm", path, r);
        return r < 0;
    }
    if (!recurse) {
        dprintf(STDERR_FILENO, "rm: %s: is a directory (rm -r removes it, rmdir an empty one)\n", path);
        return 1;
    }
    if (is_dot(path)) {
        dprintf(STDERR_FILENO, "rm: %s: will not remove '.', '..' or '/'\n", path);
        return 1;
    }
    uint32_t n = 0;
    dirent_t* list = read_dir(path, &n, &r);
    if (!list) {
        fs_error("rm", path, r);
        return 1;
    }
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        char child[PATH_MAX];
        if (join_path(child, sizeof child, path, list[i].name) < 0) {
            dprintf(STDERR_FILENO, "rm: %s/%s: the path is too long\n", path, list[i].name);
            bad = 1;
        } else {
            bad |= remove_one(child);
        }
    }
    free(list);
    if (bad) return 1;  // something stayed, so the directory is not empty
    r = sys_rmdir(path);
    if (r == -EBUSY) dprintf(STDERR_FILENO, "rm: %s: in use (someone's current directory)\n", path);
    else if (r < 0) fs_error("rm", path, r);
    return r < 0;
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "frR", got)) argc = 0;
    force = has_flag(got, 'f');
    recurse = has_flag(got, 'r') || has_flag(got, 'R');
    if (argc < 2) {
        if (argc && force) return 0;
        dprintf(STDERR_FILENO, "usage: rm [-f] [-r] FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) bad |= remove_one(argv[i]);
    return bad;
}

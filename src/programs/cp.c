// cp [-r] [-f | -n] SRC DST, or cp [-r] [-f | -n] SRC... DIR    copy files
//
// A file already at the target is replaced (-f says so; -n leaves it
// alone and says nothing). -r (or -R) also copies directories, with
// everything inside; one copied onto an existing directory merges into
// it. A directory cannot be copied into itself.
#include "ulib.h"

static char buf[4096];
static int recurse, keep;

static int fail(const char* what, const char* why) {
    dprintf(STDERR_FILENO, "cp: %s: %s\n", what, why);
    return 1;
}

static int copy_file(const char* src, const char* to, const dirent_t* s) {
    dirent_t d;
    int existed = sys_stat(to, &d) == 0;
    if (existed && (d.flags & DIRENT_DIR)) return fail(to, "is a directory");
    if (existed && d.id == s->id) return fail(to, "is the same file");
    if (existed && keep) return 0;

    int in = sys_open(src, O_RDONLY);
    if (in < 0) return fail(src, strerror(in));
    int out = sys_open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        sys_close(in);
        if (out == -EINVAL && !name_ok(to)) {
            fs_error("cp", to, out);
            return 1;
        }
        return fail(to, strerror(out));
    }
    int n, bad = 0;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        int w = sys_write(out, buf, (uint32_t)n);
        if (w != n) {
            bad = fail(to, strerror(w < 0 ? w : -ENOSPC));
            break;
        }
    }
    if (n < 0) bad = fail(src, strerror(n));
    sys_close(in);
    sys_close(out);
    if (bad && !existed) sys_delete(to);  // no half copies left behind
    return bad;
}

static int copy_any(const char* src, const char* to);

// The contents of directory src into to, making to if need be. What is
// in src is read before to is made, so a copy made inside src is not
// itself copied.
static int copy_tree(const char* src, const char* to) {
    int r;
    uint32_t n = 0;
    dirent_t* list = read_dir(src, &n, &r);
    if (!list) return fail(src, strerror(r));
    dirent_t d;
    int bad = 0;
    if (sys_stat(to, &d) < 0) {
        r = sys_mkdir(to);
        if (r < 0) {
            free(list);
            if (r == -EINVAL && !name_ok(to)) {
                fs_error("cp", to, r);
                return 1;
            }
            return fail(to, strerror(r));
        }
    } else if (!(d.flags & DIRENT_DIR)) {
        free(list);
        return fail(to, "is not a directory");
    }
    for (uint32_t i = 0; i < n; i++) {
        char a[PATH_MAX], b[PATH_MAX];
        if (join_path(a, sizeof a, src, list[i].name) < 0 || join_path(b, sizeof b, to, list[i].name) < 0) {
            bad |= fail(list[i].name, "the path is too long");
            continue;
        }
        bad |= copy_any(a, b);
    }
    free(list);
    return bad;
}

static int copy_any(const char* src, const char* to) {
    dirent_t s;
    int r = sys_stat(src, &s);
    if (r < 0) return fail(src, strerror(r));
    if (!(s.flags & DIRENT_DIR)) return copy_file(src, to, &s);
    if (!recurse) return fail(src, "is a directory (cp -r copies it)");
    if (path_inside(to, s.id)) return fail(src, "cannot copy a directory into itself");
    return copy_tree(src, to);
}

static int copy(const char* src, const char* dst) {
    char to[PATH_MAX];
    int r = dest_path(src, dst, to, sizeof to);
    if (r < 0) return fail(dst, strerror(r));
    return copy_any(src, to);
}

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "rRfn", got) || argc < 3 || (has_flag(got, 'f') && has_flag(got, 'n'))) {
        dprintf(STDERR_FILENO, "usage: cp [-r] [-f | -n] SRC DST, or cp [-r] [-f | -n] SRC... DIR\n");
        return 2;
    }
    recurse = has_flag(got, 'r') || has_flag(got, 'R');
    keep = has_flag(got, 'n');
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

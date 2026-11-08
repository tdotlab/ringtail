#include <stdarg.h>
#include "ulib.h"

int print(const char* s) {
    return sys_write(STDOUT_FILENO, s, strlen(s));
}

int puts(const char* s) {
    int n = print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
    return n;
}

static int vdprintf(int fd, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    return sys_write(fd, buf, (uint32_t)n);
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

static int checks, failures;

int check(int ok, const char* what) {
    checks++;
    if (!ok) failures++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

int check_summary(const char* program) {
    if (failures) printf("%s: %d of %d checks FAILED\n", program, failures, checks);
    else printf("%s: all %d checks passed\n", program, checks);
    return failures ? 1 : 0;
}

int dest_path(const char* src, const char* dst, char* out, size_t size) {
    dirent_t d;
    if (sys_stat(dst, &d) < 0 || !(d.flags & DIRENT_DIR)) {
        if (strlen(dst) >= size) return -ENAMETOOLONG;
        memcpy(out, dst, strlen(dst) + 1);
        return 0;
    }
    // src's last name, without trailing slashes.
    size_t end = strlen(src);
    while (end > 1 && src[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && src[start - 1] != '/') start--;
    size_t dl = strlen(dst);
    while (dl > 1 && dst[dl - 1] == '/') dl--;
    int slash = dst[dl - 1] != '/';
    size_t n = end - start;
    if (dl + (size_t)slash + n >= size) return -ENAMETOOLONG;
    memcpy(out, dst, dl);
    if (slash) out[dl] = '/';
    memcpy(out + dl + slash, src + start, n);
    out[dl + slash + n] = 0;
    return 0;
}

int name_ok(const char* path) {
    const char* n = strrchr(path, '/');
    n = n ? n + 1 : path;
    size_t len = strlen(n);
    if (!len || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return 1;
    if (len > NAME_MAX || n[0] == ' ' || n[len - 1] == ' ' || n[len - 1] == '.') return 0;
    for (const char* p = n; *p; p++)
        if (*p < 0x20 || *p > 0x7E || strchr("\"*:<>?\\|", *p)) return 0;
    return 1;
}

void fs_error(const char* prog, const char* what, int e) {
    const char* why = e == -EINVAL && !name_ok(what) ? "not a valid file name" : strerror(e);
    dprintf(STDERR_FILENO, "%s: %s: %s\n", prog, what, why);
}

dirent_t* read_dir(const char* path, uint32_t* count, int* err) {
    dirent_t* list = 0;
    uint32_t n = 0, cap = 0;
    int cursor = 0, r = 0;
    *err = 0;
    for (;;) {
        if (n == cap) {
            uint32_t want = cap ? cap * 2 : 32;
            dirent_t* p = realloc(list, want * sizeof *list);
            if (!p) {
                r = -ENOMEM;
                break;
            }
            list = p;
            cap = want;
        }
        r = sys_readdir(path, &cursor, &list[n]);
        if (r <= 0) break;
        if (strcmp(list[n].name, ".") && strcmp(list[n].name, "..")) n++;
    }
    if (r < 0) {
        free(list);
        *err = r;
        return 0;
    }
    *count = n;
    return list;
}

int join_path(char* out, size_t size, const char* dir, const char* name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = dl && dir[dl - 1] != '/';
    if (dl + (size_t)slash + nl >= size) return -ENAMETOOLONG;
    memcpy(out, dir, dl);
    if (slash) out[dl] = '/';
    memcpy(out + dl + slash, name, nl + 1);
    return 0;
}

int path_inside(const char* dir_path, uint32_t id) {
    char p[PATH_MAX];
    if (strlen(dir_path) >= sizeof p) return 0;
    memcpy(p, dir_path, strlen(dir_path) + 1);
    dirent_t d;
    // Start from the nearest part that exists: cut components off the
    // end until stat works, then climb with "..".
    for (int guard = 0; sys_stat(p, &d) < 0; guard++) {
        char* s = strrchr(p, '/');
        if (guard > 64) return 0;
        if (!s) {
            memcpy(p, ".", 2);
            break;
        }
        if (s == p) { s[1] = 0; break; }
        *s = 0;
    }
    uint32_t last = 0xFFFFFFFFu;
    for (int up = 0; up < 64; up++) {
        if (sys_stat(p, &d) < 0) return 0;
        if (d.id == id) return 1;
        if (up && d.id == last) return 0;  // the root: ".." of itself
        last = d.id;
        size_t l = strlen(p);
        if (l + 4 >= sizeof p) return 0;
        memcpy(p + l, l && p[l - 1] == '/' ? ".." : "/..", 4);
    }
    return 0;
}

int getflags(int* argc, char** argv, const char* known, char* got) {
    int n = 1, only_ops = 0, ng = 0;
    got[0] = 0;
    for (int i = 1; i < *argc; i++) {
        const char* s = argv[i];
        if (!only_ops && !strcmp(s, "--")) {
            only_ops = 1;
            continue;
        }
        if (!only_ops && s[0] == '-' && s[1]) {
            for (const char* o = s + 1; *o; o++) {
                if (!strchr(known, *o)) return (unsigned char)*o;
                if (!strchr(got, *o) && ng < 31) {
                    got[ng++] = *o;
                    got[ng] = 0;
                }
            }
            continue;
        }
        argv[n++] = argv[i];
    }
    *argc = n;
    return 0;
}

// Bottom-up merge sort of the entries by name, ignoring case (the disk
// does). Stable; allocation failure leaves them as they were.
void sort_dirents(dirent_t* list, uint32_t count) {
    if (count < 2) return;
    dirent_t* tmp = malloc(count * sizeof *tmp);
    if (!tmp) return;
    dirent_t *src = list, *dst = tmp;
    for (uint32_t w = 1; w < count; w *= 2) {
        for (uint32_t lo = 0; lo < count; lo += 2 * w) {
            uint32_t mid = lo + w < count ? lo + w : count, hi = lo + 2 * w < count ? lo + 2 * w : count;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) dst[k++] = strcasecmp(src[j].name, src[i].name) < 0 ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        dirent_t* t = src;
        src = dst;
        dst = t;
    }
    if (src != list) memcpy(list, src, count * sizeof *list);
    free(tmp);
}

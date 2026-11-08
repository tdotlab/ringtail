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

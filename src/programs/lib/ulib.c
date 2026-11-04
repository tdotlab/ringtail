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

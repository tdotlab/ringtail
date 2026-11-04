#include <stdarg.h>
#include "ulib.h"

int print(const char* s) {
    return sys_print(s, strlen(s));
}

int puts(const char* s) {
    int n = print(s);
    sys_print("\n", 1);
    return n;
}

int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    return sys_print(buf, (uint32_t)n);
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

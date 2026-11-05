// String and memory routines. The compiler may emit calls to memcpy
// and memset for struct copies, so these must exist even where the
// code never names them.
#include "lib.h"
#include "ringtail/abi.h"

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    // Overlapping with dst above src: copy backwards.
    while (n--) d[n] = s[n];
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = a;
    const uint8_t* y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char*)last;
}

size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

int parse_uint(const char* s, uint32_t* out) {
    uint32_t base = 10, v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (!*s) return -1;
    for (; *s; s++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (v > (0xFFFFFFFFu - d) / base) return -1;
        v = v * base + d;
    }
    *out = v;
    return 0;
}

const char* strerror(int err) {
    if (err < 0) err = -err;
    switch (err) {
    case 0: return "success";
    case EPERM: return "not permitted";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such task";
    case EINTR: return "interrupted";
    case EIO: return "I/O error";
    case E2BIG: return "argument list too long";
    case ENOEXEC: return "not an executable";
    case EBADF: return "bad file descriptor";
    case EAGAIN: return "try again";
    case ENOMEM: return "out of memory";
    case EFAULT: return "bad address";
    case EBUSY: return "busy";
    case EEXIST: return "already exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ENFILE: return "too many files in use";
    case EMFILE: return "too many open files";
    case EFBIG: return "file too large";
    case ENOSPC: return "disk full";
    case EPIPE: return "broken pipe";
    case ENAMETOOLONG: return "name too long";
    case ENOSYS: return "no such system call";
    case ENOTEMPTY: return "directory not empty";
    }
    return "unknown error";
}

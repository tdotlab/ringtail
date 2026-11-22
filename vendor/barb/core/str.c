// Strings: see str.h.
#include "str.h"
#include <string.h>

str_t str_of(const char* p, size_t n) { str_t s = { p, n }; return s; }

str_t str_z(const char* z) {
    str_t s = { z, z ? strlen(z) : 0 };
    return s;
}

int str_eq(str_t a, str_t b) {
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

int str_eqz(str_t a, const char* z) { return str_eq(a, str_z(z)); }

int str_starts(str_t s, str_t prefix) {
    return s.n >= prefix.n && (prefix.n == 0 || memcmp(s.p, prefix.p, prefix.n) == 0);
}

size_t str_find(str_t hay, str_t needle) {
    if (needle.n == 0) return 0;
    if (needle.n > hay.n) return STR_NONE;
    for (size_t i = 0; i + needle.n <= hay.n; i++) {
        const char* q = memchr(hay.p + i, needle.p[0], hay.n - i - needle.n + 1);
        if (!q) return STR_NONE;
        i = (size_t)(q - hay.p);
        if (memcmp(hay.p + i, needle.p, needle.n) == 0) return i;
    }
    return STR_NONE;
}

str_t str_dup(arena_t* a, str_t s) {
    char* p = arena_alloc(a, s.n + 1);  // zeroed, so the NUL is there
    if (!p) return str_of(0, 0);
    if (s.n) memcpy(p, s.p, s.n);
    return str_of(p, s.n);
}

str_t str_trim(str_t s) {
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t' || s.p[0] == '\r' || s.p[0] == '\n')) { s.p++; s.n--; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' ||
                   s.p[s.n - 1] == '\r' || s.p[s.n - 1] == '\n')) s.n--;
    return s;
}

void sb_init(sb_t* b, arena_t* a) {
    b->a = a;
    b->p = 0;
    b->n = b->cap = 0;
    b->failed = 0;
}

static int sb_room(sb_t* b, size_t extra) {
    if (b->failed) return 0;
    if (extra > ((size_t)1 << 40) - b->n - 1) { b->failed = 1; return 0; }
    if (b->n + extra + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra + 1) cap *= 2;
    char* p = arena_alloc(b->a, cap);
    if (!p) { b->failed = 1; return 0; }
    if (b->n) memcpy(p, b->p, b->n);
    b->p = p;
    b->cap = cap;
    return 1;
}

void sb_put(sb_t* b, const void* p, size_t n) {
    if (n == 0 || !sb_room(b, n)) return;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

void sb_puts(sb_t* b, str_t s) { sb_put(b, s.p, s.n); }
void sb_putz(sb_t* b, const char* z) { sb_put(b, z, strlen(z)); }
void sb_putc(sb_t* b, char c) { sb_put(b, &c, 1); }

void sb_putu(sb_t* b, uint64_t v) {
    char t[24];
    int i = 24;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    sb_put(b, t + i, (size_t)(24 - i));
}

void sb_puti(sb_t* b, int64_t v) {
    if (v < 0) {
        sb_putc(b, '-');
        sb_putu(b, (uint64_t)0 - (uint64_t)v);  // right even for INT64_MIN
    } else {
        sb_putu(b, (uint64_t)v);
    }
}

void sb_pad_u(sb_t* b, uint64_t v, int width) {
    char t[24];
    int i = 24;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int k = 24 - i; k < width; k++) sb_putc(b, ' ');
    sb_put(b, t + i, (size_t)(24 - i));
}

str_t sb_str(sb_t* b) {
    if (b->failed) return str_of(0, 0);
    if (!b->p && !sb_room(b, 0)) return str_of(0, 0);
    b->p[b->n] = 0;  // sb_room always leaves one byte for this
    return str_of(b->p, b->n);
}

#include "joincore.h"

#define MAXFIELD ((uint64_t)INT64_MAX)

static int is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

#define NUM_OK 0
#define NUM_BAD 1
#define NUM_OVERFLOW 2

// What strtol makes of base 10 text that must end with the number: blanks, a
// sign, digits. A number out of range, either way, is NUM_OVERFLOW.
static int parse_number(const char* s, int64_t* out) {
    while (is_space(*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (*s < '0' || *s > '9') return NUM_BAD;
    int over = 0;
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*s) return NUM_BAD;
    if (!over && (neg ? v > (uint64_t)INT64_MAX + 1 : v > (uint64_t)INT64_MAX)) over = 1;
    if (over) return NUM_OVERFLOW;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return NUM_OK;
}

int jn_parse_field(const char* s, uint64_t* out) {
    int64_t v = 0;
    int r = parse_number(s, &v);
    if (r == NUM_OVERFLOW) {
        *out = MAXFIELD - 1;
        return 0;
    }
    if (r != NUM_OK || v <= 0) return -1;
    *out = (uint64_t)v - 1;
    return 0;
}

int jn_parse_filenum(const char* s) {
    int64_t v = 0;
    if (parse_number(s, &v) != NUM_OK) return 0;
    return v == 1 || v == 2 ? (int)v : 0;
}

int jn_decode_spec(const char* s, int* file, uint64_t* field) {
    if (s[0] == '0') {
        if (s[1]) return JN_BAD_SPEC;
        *file = 0;
        *field = 0;
        return JN_OK;
    }
    if (s[0] == '1' || s[0] == '2') {
        if (s[1] != '.') return JN_BAD_SPEC;
        if (jn_parse_field(s + 2, field)) return JN_BAD_FIELD;
        *file = s[0] - '0';
        return JN_OK;
    }
    return JN_BAD_FILE;
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\n'; }

uint32_t jn_split(const char* s, uint32_t n, int tab, jn_field_t* out, uint32_t cap) {
    uint32_t count = 0, p = 0;
#define EMIT(b, l) \
    do { \
        if (count < cap) out[count] = (jn_field_t){ (b), (l) }; \
        count++; \
    } while (0)
    if (n == 0) return 0;
    if (tab >= 0 && tab != '\n') {
        for (;;) {
            uint32_t q = p;
            while (q < n && (unsigned char)s[q] != (unsigned)tab) q++;
            if (q == n) break;
            EMIT(p, q - p);
            p = q + 1;
        }
    } else if (tab < 0) {
        while (is_blank(s[p]))
            if (++p == n) return 0;
        do {
            uint32_t q = p + 1;
            while (q != n && !is_blank(s[q])) q++;
            EMIT(p, q - p);
            if (q == n) return count;
            for (p = q + 1; p != n && is_blank(s[p]); p++) { }
        } while (p != n);
    }
    EMIT(p, n - p);
    return count;
#undef EMIT
}

static unsigned char upper(unsigned char c) { return c >= 'a' && c <= 'z' ? (unsigned char)(c - 'a' + 'A') : c; }

int jn_keycmp(const char* a, uint32_t alen, const char* b, uint32_t blen, int icase) {
    if (alen == 0) return blen == 0 ? 0 : -1;
    if (blen == 0) return 1;
    uint32_t n = alen < blen ? alen : blen;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (icase) {
            x = upper(x);
            y = upper(y);
        }
        if (x != y) return x < y ? -1 : 1;
    }
    return alen < blen ? -1 : alen > blen;
}

void jn_u64_text(uint64_t v, char* out) {
    char d[24];
    int nd = 0;
    do {
        d[nd++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (nd) *out++ = d[--nd];
    *out = 0;
}

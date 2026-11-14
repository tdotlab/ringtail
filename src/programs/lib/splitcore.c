// See splitcore.h.
#include <stdint.h>
#include <string.h>
#include "splitcore.h"

static int is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// Blanks, an optional +, digits. Returns where they end, or 0 if there are no
// digits (a - is not a digit either). *v is the number, up to the biggest one.
static const char* read_digits(const char* s, uint64_t* v) {
    const char* p = s;
    while (is_space(*p)) p++;
    if (*p == '+') p++;
    if (*p < '0' || *p > '9') return 0;
    uint64_t n = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        uint64_t d = (uint64_t)(*p - '0');
        n = n > (UINT64_MAX - d) / 10 ? UINT64_MAX : n * 10 + d;
    }
    *v = n;
    return p;
}

static uint64_t sat_mul(uint64_t a, uint64_t b) { return a > UINT64_MAX / b ? UINT64_MAX : a * b; }

int sp_parse_lines(const char* s, uint64_t* out) {
    uint64_t v;
    const char* p = read_digits(s, &v);
    if (!p || *p || v == 0) return -1;
    *out = v;
    return 0;
}

int sp_parse_bytes(const char* s, uint64_t* out) {
    static const char SUFFIXES[] = "bEGKkMmPQRTYZ";
    uint64_t v = 1;
    const char* p = read_digits(s, &v);
    if (!p) {
        // No number: a suffix at the very start stands for 1 of it.
        if (!*s || !strchr(SUFFIXES, *s)) return -1;
        v = 1;
        p = s;
    }
    if (*p) {
        char c = *p++;
        if (!strchr(SUFFIXES, c)) return -1;
        if (c == 'b') {
            v = sat_mul(v, 512);
        } else {
            uint64_t base = 1024;
            if (p[0] == 'i' && p[1] == 'B') {
                p += 2;
            } else if (p[0] == 'B' || p[0] == 'D') {
                base = 1000;
                p++;
            }
            int power = 0;
            switch (c) {
                case 'K': case 'k': power = 1; break;
                case 'M': case 'm': power = 2; break;
                case 'G': power = 3; break;
                case 'T': power = 4; break;
                case 'P': power = 5; break;
                case 'E': power = 6; break;
                case 'Z': power = 7; break;
                case 'Y': power = 8; break;
                case 'R': power = 9; break;
                default: power = 10; break;  // Q
            }
            while (power--) v = sat_mul(v, base);
        }
        if (*p) return -1;
    }
    if (v == 0) return -1;
    *out = v;
    return 0;
}

int sp_parse_suffix_len(const char* s, uint32_t* out) {
    const char* p = s;
    while (is_space(*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (*p < '0' || *p > '9') return -1;
    uint64_t n = 0;
    int over = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        uint64_t d = (uint64_t)(*p - '0');
        if (n > (UINT64_MAX - d) / 10) over = 1;
        else n = n * 10 + d;
    }
    if (*p) return -1;
    // GNU reads a long: 64 bits. Of the negative numbers it calls only those
    // down to -2^30 out of range; the ones below are "too large".
    if (over || n > (uint64_t)INT64_MAX) return -3;
    if (neg && n) return n <= (1u << 30) ? -2 : -3;
    *out = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    return 0;
}

static int digit_value(char c, int kind) {
    if (c >= '0' && c <= '9') return c - '0';
    if (kind == SP_HEX && c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int sp_suffix_init(sp_suffix_t* s, int kind, uint32_t len, const char* from) {
    memset(s, 0, sizeof *s);
    s->base = kind == SP_ALPHA ? 26 : kind == SP_NUM ? 10 : 16;
    s->widen = len == 0 && !from;
    if (len > SP_MAXLEN) return SP_TOO_LONG;
    s->len = len ? (int)len : 2;
    if (from) {
        if (kind == SP_ALPHA || !*from) return SP_BAD_START;
        int flen = 0;
        for (; from[flen]; flen++)
            if (digit_value(from[flen], kind) < 0) return SP_BAD_START;
        int skip = 0;
        while (skip < flen - 1 && from[skip] == '0') skip++;
        int digits = flen - skip;
        if (digits == 1 && from[skip] == '0') digits = 0;
        if (digits > s->len) return SP_START_TOO_LARGE;
        for (int i = 0; i < digits; i++) s->dig[s->len - digits + i] = (unsigned char)digit_value(from[flen - digits + i], kind);
    }
    return SP_OK;
}

void sp_suffix_text(const sp_suffix_t* s, char* out) {
    for (int i = 0; i < s->len; i++) {
        int d = s->dig[i];
        out[i] = (char)(s->base == 26 ? 'a' + d : d < 10 ? '0' + d : 'a' + d - 10);
    }
    out[s->len] = 0;
}

int sp_suffix_next(sp_suffix_t* s) {
    int last = s->base - 1;
    for (int i = s->len - 1; i >= s->markers; i--) {
        if (i == s->markers && s->widen) {
            // The first letter after the markers may not reach the last
            // symbol: that is where the suffix grows by a marker and a letter.
            if (s->dig[i] < last - 1) {
                s->dig[i]++;
                return 0;
            }
            if (s->len + 2 > SP_MAXLEN) return -1;
            s->markers++;
            s->len += 2;
            for (int j = 0; j < s->len; j++) s->dig[j] = (unsigned char)(j < s->markers ? last : 0);
            return 0;
        }
        if (s->dig[i] < last) {
            s->dig[i]++;
            return 0;
        }
        s->dig[i] = 0;
    }
    return -1;
}

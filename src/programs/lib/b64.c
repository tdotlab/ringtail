#include "b64.h"

static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0..63 for a character of the alphabet, -1 for anything else.
static int value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void b64enc_init(b64enc_t* e, uint32_t wrap) {
    e->n = 0;
    e->wrap = wrap;
    e->col = 0;
}

static int emit(b64enc_t* e, char* out, int k, char c) {
    out[k++] = c;
    if (e->wrap && ++e->col == e->wrap) {
        out[k++] = '\n';
        e->col = 0;
    }
    return k;
}

int b64enc_put(b64enc_t* e, uint8_t c, char* out) {
    e->in[e->n++] = c;
    if (e->n < 3) return 0;
    e->n = 0;
    int k = 0;
    k = emit(e, out, k, ALPHA[e->in[0] >> 2]);
    k = emit(e, out, k, ALPHA[((e->in[0] & 3) << 4) | (e->in[1] >> 4)]);
    k = emit(e, out, k, ALPHA[((e->in[1] & 15) << 2) | (e->in[2] >> 6)]);
    k = emit(e, out, k, ALPHA[e->in[2] & 63]);
    return k;
}

int b64enc_end(b64enc_t* e, char* out) {
    int k = 0;
    if (e->n) {
        uint8_t b1 = e->n == 2 ? e->in[1] : 0;
        k = emit(e, out, k, ALPHA[e->in[0] >> 2]);
        k = emit(e, out, k, ALPHA[((e->in[0] & 3) << 4) | (b1 >> 4)]);
        k = emit(e, out, k, e->n == 2 ? ALPHA[(b1 & 15) << 2] : '=');
        k = emit(e, out, k, '=');
        e->n = 0;
    }
    if (e->wrap && e->col) {
        out[k++] = '\n';
        e->col = 0;
    }
    return k;
}

void b64dec_init(b64dec_t* d, int ignore_garbage) {
    d->n = 0;
    d->ignore = ignore_garbage;
    d->bad = 0;
}

// What GNU base64 makes of one group of 1 to 4 characters: the bytes it
// writes (it writes each as soon as the characters for it are good) and
// whether the group was whole and valid. Padding is allowed only at the
// end of a group of four, as "xx==" or "xxx=".
static int group(const char* q, int n, uint8_t* out, int* ok) {
    *ok = 0;
    if (n < 2) return 0;
    int v0 = value((uint8_t)q[0]), v1 = value((uint8_t)q[1]);
    if (v0 < 0 || v1 < 0) return 0;
    out[0] = (uint8_t)((v0 << 2) | (v1 >> 4));
    if (n == 2) return 1;
    if (q[2] == '=') {
        *ok = n == 4 && q[3] == '=';
        return 1;
    }
    int v2 = value((uint8_t)q[2]);
    if (v2 < 0) return 1;
    out[1] = (uint8_t)(((v1 << 4) & 0xf0) | (v2 >> 2));
    if (n == 3) return 2;
    if (q[3] == '=') {
        *ok = 1;
        return 2;
    }
    int v3 = value((uint8_t)q[3]);
    if (v3 < 0) return 2;
    out[2] = (uint8_t)(((v2 << 6) & 0xc0) | v3);
    *ok = 1;
    return 3;
}

int b64dec_put(b64dec_t* d, uint8_t c, uint8_t* out) {
    if (c == '\n') return 0;
    if (d->ignore && c != '=' && value(c) < 0) return 0;
    d->q[d->n++] = (char)c;
    if (d->n < 4) return 0;
    int ok;
    int k = group(d->q, 4, out, &ok);
    d->n = 0;
    if (!ok) d->bad = 1;
    return k;
}

int b64dec_end(b64dec_t* d, uint8_t* out) {
    if (!d->n) return 0;
    int ok;
    int k = group(d->q, d->n, out, &ok);
    d->n = 0;
    if (!ok) d->bad = 1;
    return k;
}

static int is_blank(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int b64_parse_wrap(const char* s, uint32_t* wrap) {
    while (is_blank(*s)) s++;
    int minus = 0;
    if (*s == '+') s++;
    else if (*s == '-') {
        minus = 1;
        s++;
    }
    if (*s < '0' || *s > '9') return -1;
    uint64_t v = 0;
    int nonzero = 0, overflow = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        uint64_t digit = (uint64_t)(*s - '0');
        if (digit) nonzero = 1;
        // GNU reads the number as a signed 64-bit one; one too big for that
        // acts as 0 (no line breaks and no last newline), one that merely
        // does not fit in 32 bits as the longest line there could be.
        if (v > (0x7FFFFFFFFFFFFFFFull - digit) / 10) overflow = 1;
        else v = v * 10 + digit;
    }
    if (*s) return -1;
    if (minus && nonzero) return -1;  // a minus sign is fine on a zero, as in GNU
    if (overflow) v = 0;
    else if (v > 0xFFFFFFFFu) v = 0xFFFFFFFFu;
    *wrap = (uint32_t)v;
    return 0;
}

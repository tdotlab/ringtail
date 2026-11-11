// Reading DER (X.690), the encoding certificates and signatures come in:
// a tag, a length, and that many bytes of contents, nested. Strictly:
// DER allows exactly one encoding of each value, and anything else
// (a length in more bytes than it needs, a length with no end, an
// integer with a needless leading byte) is refused, because two parsers
// that disagree about what a certificate says are how forgeries pass.
#ifndef RINGTAIL_DER_H
#define RINGTAIL_DER_H

#include <stdint.h>

// Tags (with the "constructed" bit where it belongs).
#define DER_BOOLEAN      0x01
#define DER_INTEGER      0x02
#define DER_BIT_STRING   0x03
#define DER_OCTET_STRING 0x04
#define DER_NULL         0x05
#define DER_OID          0x06
#define DER_UTF8         0x0c
#define DER_PRINTABLE    0x13
#define DER_IA5          0x16
#define DER_UTC_TIME     0x17
#define DER_GEN_TIME     0x18
#define DER_SEQUENCE     0x30
#define DER_SET          0x31
#define DER_CONTEXT(n)   (0xa0 | (n))  // [n], constructed
#define DER_IMPLICIT(n)  (0x80 | (n))  // [n], primitive

// A stretch of DER still to be read. Reading past the end, or anything
// malformed, sets bad (and from then on every read fails), so a parser
// may read on and check bad once.
typedef struct {
    const uint8_t* p;
    uint32_t n;
    int bad;
} der_t;

static inline der_t der_of(const uint8_t* p, uint32_t n) { return (der_t){ p, n, 0 }; }

// The next element: its tag (one byte; tags past 30 are refused) and its
// contents. 0, or -1 (and bad set) when there is none or it is malformed.
static inline int der_next(der_t* d, uint8_t* tag, der_t* contents) {
    *contents = (der_t){ 0, 0, 1 };
    if (d->bad || d->n < 2 || (d->p[0] & 0x1f) == 0x1f) goto bad;
    uint32_t at = 2, len = d->p[1];
    if (len & 0x80) {
        uint32_t k = len & 0x7f;
        if (k == 0 || k > 3 || d->n < 2 + k || d->p[2] == 0) goto bad;  // no end, too long, or a needless 0
        len = 0;
        for (uint32_t i = 0; i < k; i++) len = len << 8 | d->p[2 + i];
        if (len < 0x80) goto bad;  // would have fitted in one byte
        at += k;
    }
    if (d->n - at < len) goto bad;
    *tag = d->p[0];
    *contents = (der_t){ d->p + at, len, 0 };
    d->p += at + len;
    d->n -= at + len;
    return 0;
bad:
    d->bad = 1;
    d->n = 0;
    return -1;
}

// The next element, which must have this tag.
static inline int der_expect(der_t* d, uint8_t tag, der_t* contents) {
    uint8_t t;
    if (der_next(d, &t, contents) != 0) return -1;
    if (t != tag) {
        d->bad = 1;
        d->n = 0;
        *contents = (der_t){ 0, 0, 1 };
        return -1;
    }
    return 0;
}

// Whether the next element has this tag (nothing is read).
static inline int der_peek(const der_t* d, uint8_t tag) { return !d->bad && d->n && d->p[0] == tag; }

// A non-negative INTEGER: its magnitude, without the leading zero DER
// puts before a top bit that is set. Refuses negative numbers and
// needless leading bytes. (Zero comes back as one zero byte.)
static inline int der_uint(der_t* d, const uint8_t** p, uint32_t* n) {
    der_t c;
    if (der_expect(d, DER_INTEGER, &c) != 0) return -1;
    if (c.n == 0 || (c.p[0] & 0x80) || (c.n > 1 && c.p[0] == 0 && !(c.p[1] & 0x80))) {
        d->bad = 1;
        d->n = 0;
        return -1;
    }
    if (c.n > 1 && c.p[0] == 0) {
        c.p++;
        c.n--;
    }
    *p = c.p;
    *n = c.n;
    return 0;
}

#endif

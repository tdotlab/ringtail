// X25519: see x25519.h.
//
// The arithmetic is modulo p = 2^255 - 19. A number is sixteen 16-bit
// pieces, each held in a 64-bit integer: there is so much headroom that
// additions need no carrying and a whole multiplication can be summed
// before anything is carried, which keeps the code short enough to
// check by eye. (The layout is the one TweetNaCl made well known.) It
// is not the fastest way, and at a few milliseconds a key exchange does
// not need to be.
//
// Everything here takes the same steps whatever the secret is: no
// branch and no array index depends on it.
#include "x25519.h"
#include "ct.h"
#include "lib.h"

typedef int64_t fe[16];

// Brings every piece back to 0..65535, passing what is over to the next
// one up; what falls off the top is worth 2^256 = 38 and comes back in
// at the bottom. Pieces may be negative going in: >> on a negative
// number rounds down here (gcc), so the carry is then negative too.
static void carry(fe o) {
    for (int i = 0; i < 16; i++) {
        int64_t c = o[i] >> 16;
        o[i] -= c * 65536;
        if (i < 15) o[i + 1] += c;
        else o[0] += 38 * c;
    }
}

static void add(fe o, const fe a, const fe b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void sub(fe o, const fe a, const fe b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

// Inputs may be sums or differences of carried numbers (pieces within
// about +-2^18): each of the 256 products is then under 2^36 and a
// column of 16 of them, times 38, under 2^46.
static void mul(fe o, const fe a, const fe b) {
    int64_t t[31] = { 0 };
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    carry(o);
    carry(o);
    ct_wipe(t, sizeof t);
}

static void sqr(fe o, const fe a) { mul(o, a, a); }

// Swaps a and b when bit is 1, touching both the same way when it is 0.
static void cswap(fe a, fe b, int64_t bit) {
    int64_t mask = -bit;
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static void from_bytes(fe o, const uint8_t in[32]) {
    for (int i = 0; i < 16; i++) o[i] = in[2 * i] | (int64_t)in[2 * i + 1] << 8;
    o[15] &= 0x7fff;  // the top bit is not part of the number
}

// The one value in 0..p-1, as bytes. After three carries the number is
// below 2^256, so less than 2p + 38; subtracting p twice, each time
// only if the result stays non-negative, finishes the job.
static void to_bytes(uint8_t out[32], const fe n) {
    fe t, m;
    memcpy(t, n, sizeof t);
    carry(t);
    carry(t);
    carry(t);
    for (int pass = 0; pass < 2; pass++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);  // borrow
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t negative = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        cswap(t, m, 1 - negative);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)t[i];
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
    ct_wipe(t, sizeof t);
    ct_wipe(m, sizeof m);
}

// 1/a, as a^(p-2): p - 2 is 2^255 - 21, every bit set but bits 2 and 4.
static void invert(fe o, const fe a) {
    fe c;
    memcpy(c, a, sizeof c);
    for (int i = 253; i >= 0; i--) {
        sqr(c, c);
        if (i != 2 && i != 4) mul(c, c, a);
    }
    memcpy(o, c, sizeof c);
    ct_wipe(c, sizeof c);
}

// The Montgomery ladder, step for step as RFC 7748 section 5 gives it.
static void scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    static const fe a24 = { 0xdb41, 1 };  // 121665
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] = (k[31] & 127) | 64;
    fe x1, x2 = { 1 }, z2 = { 0 }, x3, z3 = { 1 };
    fe a, aa, b, bb, e, c, d, da, cb, t;
    from_bytes(x1, point);
    memcpy(x3, x1, sizeof x3);
    int64_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        int64_t bit = (k[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        cswap(x2, x3, swap);
        cswap(z2, z3, swap);
        swap = bit;
        add(a, x2, z2);
        sqr(aa, a);
        sub(b, x2, z2);
        sqr(bb, b);
        sub(e, aa, bb);
        add(c, x3, z3);
        sub(d, x3, z3);
        mul(da, d, a);
        mul(cb, c, b);
        add(t, da, cb);
        sqr(x3, t);
        sub(t, da, cb);
        sqr(t, t);
        mul(z3, x1, t);
        mul(x2, aa, bb);
        mul(t, a24, e);
        add(t, aa, t);
        mul(z2, e, t);
    }
    cswap(x2, x3, swap);
    cswap(z2, z3, swap);
    invert(z2, z2);
    mul(x2, x2, z2);
    to_bytes(out, x2);
    ct_wipe(k, sizeof k);
    ct_wipe(x1, sizeof x1); ct_wipe(x2, sizeof x2); ct_wipe(z2, sizeof z2); ct_wipe(x3, sizeof x3); ct_wipe(z3, sizeof z3);
    ct_wipe(a, sizeof a); ct_wipe(aa, sizeof aa); ct_wipe(b, sizeof b); ct_wipe(bb, sizeof bb); ct_wipe(e, sizeof e);
    ct_wipe(c, sizeof c); ct_wipe(d, sizeof d); ct_wipe(da, sizeof da); ct_wipe(cb, sizeof cb); ct_wipe(t, sizeof t);
}

void x25519_public(uint8_t pub[X25519_LEN], const uint8_t secret[X25519_LEN]) {
    static const uint8_t base[32] = { 9 };
    scalarmult(pub, secret, base);
}

int x25519(uint8_t shared[X25519_LEN], const uint8_t secret[X25519_LEN], const uint8_t their_pub[X25519_LEN]) {
    scalarmult(shared, secret, their_pub);
    uint32_t any = 0;
    for (int i = 0; i < X25519_LEN; i++) any |= shared[i];
    return (int)(((any - 1) >> 8) & 1) ? -1 : 0;
}

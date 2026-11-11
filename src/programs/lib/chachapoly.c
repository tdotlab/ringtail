// Poly1305 and the ChaCha20-Poly1305 AEAD: see chachapoly.h.
//
// Poly1305 works on numbers modulo 2^130 - 5. Here such a number is five
// 26-bit pieces, so that the product of two pieces fits in 64 bits with
// room to add five of them up. Nothing in this file branches on, or
// indexes by, secret data.
#include "chachapoly.h"
#include "chacha20.h"
#include "ct.h"
#include "lib.h"

static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void poly1305_init(poly1305_t* p, const uint8_t key[32]) {
    // r, with the bits the definition says to clear.
    p->r[0] = le32(key) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
    p->have = 0;
}

// h = (h + block) * r for each 16-byte block. hibit is the 2^128 that
// goes on top of a whole block (the last, short block carries its own
// marker byte instead).
static void blocks(poly1305_t* p, const uint8_t* m, size_t len, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;  // 2^130 is 5, so the high pieces fold in times 5
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    for (; len >= 16; m += 16, len -= 16) {
        h0 += le32(m) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        // Carry, leaving each piece at 26 bits or barely over.
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

void poly1305_update(poly1305_t* p, const void* data, size_t len) {
    const uint8_t* m = data;
    if (!len) return;
    if (p->have) {
        size_t take = 16 - p->have < len ? 16 - p->have : len;
        memcpy(p->buf + p->have, m, take);
        p->have += (uint32_t)take;
        m += take;
        len -= take;
        if (p->have < 16) return;
        blocks(p, p->buf, 16, 1u << 24);
        p->have = 0;
    }
    size_t whole = len & ~(size_t)15;
    blocks(p, m, whole, 1u << 24);
    if (len - whole) {
        memcpy(p->buf, m + whole, len - whole);
        p->have = (uint32_t)(len - whole);
    }
}

void poly1305_final(poly1305_t* p, uint8_t tag[16]) {
    if (p->have) {
        p->buf[p->have] = 1;
        memset(p->buf + p->have + 1, 0, 16 - p->have - 1);
        blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    // h is now below 2^130, so at most one p too big. g = h - p; keep it
    // if that did not go negative. Chosen by mask, not by branch.
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t keep_g = (g4 >> 31) - 1;  // all ones when h >= p
    uint32_t keep_h = ~keep_g;
    h0 = (h0 & keep_h) | (g0 & keep_g);
    h1 = (h1 & keep_h) | (g1 & keep_g);
    h2 = (h2 & keep_h) | (g2 & keep_g);
    h3 = (h3 & keep_h) | (g3 & keep_g);
    h4 = (h4 & keep_h) | (g4 & keep_g);

    // Back to four 32-bit words, add the second half of the key, and
    // keep the low 128 bits.
    uint32_t w0 = h0 | h1 << 26;
    uint32_t w1 = h1 >> 6 | h2 << 20;
    uint32_t w2 = h2 >> 12 | h3 << 14;
    uint32_t w3 = h3 >> 18 | h4 << 8;
    uint64_t f = (uint64_t)w0 + p->pad[0]; w0 = (uint32_t)f;
    f = (uint64_t)w1 + p->pad[1] + (f >> 32); w1 = (uint32_t)f;
    f = (uint64_t)w2 + p->pad[2] + (f >> 32); w2 = (uint32_t)f;
    f = (uint64_t)w3 + p->pad[3] + (f >> 32); w3 = (uint32_t)f;
    uint32_t w[4] = { w0, w1, w2, w3 };
    for (int i = 0; i < 4; i++) {
        tag[4 * i] = (uint8_t)w[i];
        tag[4 * i + 1] = (uint8_t)(w[i] >> 8);
        tag[4 * i + 2] = (uint8_t)(w[i] >> 16);
        tag[4 * i + 3] = (uint8_t)(w[i] >> 24);
    }
    ct_wipe(p, sizeof *p);
}

// The tag over aad and cipher text, each padded with zeros to a
// multiple of 16 bytes, then both lengths as 64-bit numbers. The
// Poly1305 key is the first 32 bytes of key stream block 0; the data
// is encrypted from block 1 on.
static void make_tag(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aadlen,
                     const uint8_t* cipher, size_t len, uint8_t tag[16]) {
    static const uint8_t zeros[16];
    uint8_t otk[32];
    chacha20_xor(key, 0, nonce, 0, otk, sizeof otk);
    poly1305_t p;
    poly1305_init(&p, otk);
    poly1305_update(&p, aad, aadlen);
    poly1305_update(&p, zeros, (16 - aadlen % 16) % 16);
    poly1305_update(&p, cipher, len);
    poly1305_update(&p, zeros, (16 - len % 16) % 16);
    uint8_t lens[16] = { 0 };
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)((uint64_t)aadlen >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    poly1305_update(&p, lens, sizeof lens);
    poly1305_final(&p, tag);
    ct_wipe(otk, sizeof otk);
}

void chachapoly_seal(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aadlen,
                     const uint8_t* plain, size_t len, uint8_t* cipher, uint8_t tag[16]) {
    if (len) chacha20_xor(key, 1, nonce, plain, cipher, len);
    make_tag(key, nonce, aad, aadlen, cipher, len, tag);
}

int chachapoly_open(const uint8_t key[32], const uint8_t nonce[12], const void* aad, size_t aadlen,
                    const uint8_t* cipher, size_t len, const uint8_t tag[16], uint8_t* plain) {
    uint8_t want[16];
    make_tag(key, nonce, aad, aadlen, cipher, len, want);
    int ok = ct_equal(want, tag, sizeof want);
    ct_wipe(want, sizeof want);
    if (!ok) return -1;
    if (len) chacha20_xor(key, 1, nonce, cipher, plain, len);
    return 0;
}

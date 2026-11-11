// AES-128-GCM. See aesgcm.h.
//
// AES works here on two blocks at once, bitsliced: q[i] holds bit i of
// each of the 32 bytes, byte p at bit p (bytes 0 to 15 the first block,
// 16 to 31 the second, each in AES's own order, column after column).
// Then SubBytes is one Boolean circuit run on whole words, ShiftRows a
// fixed shuffle of bit positions, and MixColumns rotations within each
// column's four bits plus a multiply by x done by moving whole planes.
// None of it depends on what the bytes are.
#include "aesgcm.h"

#include <string.h>
#include "ct.h"

// ---- Moving between bytes and planes ----

// Bit i of four bytes (a little-endian word) as four bits in a row.
static uint32_t gather(uint32_t w, int i) {
    uint32_t x = (w >> i) & 0x01010101;
    x |= x >> 7;
    x |= x >> 14;
    return x & 0xF;
}

// The reverse: four bits in a row as bit i of four bytes.
static uint32_t scatter(uint32_t n, int i) {
    uint32_t x = n & 0xF;
    x = (x | x << 14) & 0x00030003;
    x = (x | x << 7) & 0x01010101;
    return x << i;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void load(uint32_t q[8], const uint8_t in[32]) {
    for (int i = 0; i < 8; i++) q[i] = 0;
    for (int w = 0; w < 8; w++) {
        uint32_t v = le32(in + 4 * w);
        for (int i = 0; i < 8; i++) q[i] |= gather(v, i) << (4 * w);
    }
}

static void store(const uint32_t q[8], uint8_t out[32]) {
    for (int w = 0; w < 8; w++) {
        uint32_t v = 0;
        for (int i = 0; i < 8; i++) v |= scatter(q[i] >> (4 * w), i);
        put_le32(out + 4 * w, v);
    }
}

// ---- The round functions ----

// SubBytes: the S-box as the 113-gate circuit of Boyar and Peralta ("A
// depth-16 circuit for the AES S-box", 2011; NOTs folded into the last
// XORs), on 32 bytes at once. x0 is each byte's top bit.
static void sub_bytes(uint32_t q[8]) {
    uint32_t x0 = q[7], x1 = q[6], x2 = q[5], x3 = q[4], x4 = q[3], x5 = q[2], x6 = q[1], x7 = q[0];
    // The linear layer going in.
    uint32_t y14 = x3 ^ x5, y13 = x0 ^ x6, y9 = x0 ^ x3, y8 = x0 ^ x5, t0 = x1 ^ x2;
    uint32_t y1 = t0 ^ x7, y4 = y1 ^ x3, y12 = y13 ^ y14, y2 = y1 ^ x0, y5 = y1 ^ x6;
    uint32_t y3 = y5 ^ y8, t1 = x4 ^ y12, y15 = t1 ^ x5, y20 = t1 ^ x1, y6 = y15 ^ x7;
    uint32_t y10 = y15 ^ t0, y11 = y20 ^ y9, y7 = x7 ^ y11, y17 = y10 ^ y11, y19 = y10 ^ y8;
    uint32_t y16 = t0 ^ y11, y21 = y13 ^ y16, y18 = x0 ^ y16;
    // The inversion in GF(2^8), by way of GF(2^4).
    uint32_t t2 = y12 & y15, t3 = y3 & y6, t4 = t3 ^ t2, t5 = y4 & x7, t6 = t5 ^ t2;
    uint32_t t7 = y13 & y16, t8 = y5 & y1, t9 = t8 ^ t7, t10 = y2 & y7, t11 = t10 ^ t7;
    uint32_t t12 = y9 & y11, t13 = y14 & y17, t14 = t13 ^ t12, t15 = y8 & y10, t16 = t15 ^ t12;
    uint32_t t17 = t4 ^ t14, t18 = t6 ^ t16, t19 = t9 ^ t14, t20 = t11 ^ t16;
    uint32_t t21 = t17 ^ y20, t22 = t18 ^ y19, t23 = t19 ^ y21, t24 = t20 ^ y18;
    uint32_t t25 = t21 ^ t22, t26 = t21 & t23, t27 = t24 ^ t26, t28 = t25 & t27, t29 = t28 ^ t22;
    uint32_t t30 = t23 ^ t24, t31 = t22 ^ t26, t32 = t31 & t30, t33 = t32 ^ t24, t34 = t23 ^ t33;
    uint32_t t35 = t27 ^ t33, t36 = t24 & t35, t37 = t36 ^ t34, t38 = t27 ^ t36, t39 = t29 & t38;
    uint32_t t40 = t25 ^ t39;
    uint32_t t41 = t40 ^ t37, t42 = t29 ^ t33, t43 = t29 ^ t40, t44 = t33 ^ t37, t45 = t42 ^ t41;
    uint32_t z0 = t44 & y15, z1 = t37 & y6, z2 = t33 & x7, z3 = t43 & y16, z4 = t40 & y1, z5 = t29 & y7;
    uint32_t z6 = t42 & y11, z7 = t45 & y17, z8 = t41 & y10, z9 = t44 & y12, z10 = t37 & y3, z11 = t33 & y4;
    uint32_t z12 = t43 & y13, z13 = t40 & y5, z14 = t29 & y2, z15 = t42 & y9, z16 = t45 & y14, z17 = t41 & y8;
    // The linear layer going out, the affine constant included.
    uint32_t t46 = z15 ^ z16, t47 = z10 ^ z11, t48 = z5 ^ z13, t49 = z9 ^ z10, t50 = z2 ^ z12;
    uint32_t t51 = z2 ^ z5, t52 = z7 ^ z8, t53 = z0 ^ z3, t54 = z6 ^ z7, t55 = z16 ^ z17;
    uint32_t t56 = z12 ^ t48, t57 = t50 ^ t53, t58 = z4 ^ t46, t59 = z3 ^ t54, t60 = t46 ^ t57;
    uint32_t t61 = z14 ^ t57, t62 = t52 ^ t58, t63 = t49 ^ t58, t64 = z4 ^ t59, t65 = t61 ^ t62;
    uint32_t t66 = z1 ^ t63, t67 = t64 ^ t65;
    uint32_t s0 = t59 ^ t63, s6 = t56 ^ ~t62, s7 = t48 ^ ~t60, s3 = t53 ^ t66, s4 = t51 ^ t66;
    uint32_t s5 = t47 ^ t65, s1 = t64 ^ ~s3, s2 = t55 ^ ~t67;
    q[7] = s0, q[6] = s1, q[5] = s2, q[4] = s3, q[3] = s4, q[2] = s5, q[1] = s6, q[0] = s7;
}

// Each 16-bit half of x, rotated right by k bits.
static uint32_t ror_halves(uint32_t x, int k) {
    uint32_t lo = (0xFFFFu >> k) * 0x10001u, hi = ((0xFFFFu << (16 - k)) & 0xFFFF) * 0x10001u;
    return ((x >> k) & lo) | ((x << (16 - k)) & hi);
}

// ShiftRows: row r (the bytes at r, r+4, r+8, r+12) moves left r
// columns, so byte r+4c takes the one at r+4(c+r): a rotation by 4r bits.
static void shift_rows(uint32_t q[8]) {
    for (int i = 0; i < 8; i++) {
        uint32_t x = q[i];
        q[i] = (x & 0x11111111) | ror_halves(x & 0x22222222, 4) | ror_halves(x & 0x44444444, 8) |
               ror_halves(x & 0x88888888, 12);
    }
}

// Within each column (four bits in a row), byte r takes byte r+k's bit.
static uint32_t col_rot1(uint32_t x) { return ((x >> 1) & 0x77777777) | ((x << 3) & 0x88888888); }
static uint32_t col_rot2(uint32_t x) { return ((x >> 2) & 0x33333333) | ((x << 2) & 0xCCCCCCCC); }
static uint32_t col_rot3(uint32_t x) { return ((x >> 3) & 0x11111111) | ((x << 1) & 0xEEEEEEEE); }

// MixColumns: b[r] = 2a[r] + 3a[r+1] + a[r+2] + a[r+3]
//                  = 2(a[r] + a[r+1]) + a[r+1] + a[r+2] + a[r+3].
// Multiplying by 2 (x, modulo x^8+x^4+x^3+x+1) moves each plane up one
// and folds the top plane back into planes 0, 1, 3 and 4.
static void mix_columns(uint32_t q[8]) {
    uint32_t t[8], rest[8];
    for (int i = 0; i < 8; i++) {
        uint32_t r1 = col_rot1(q[i]);
        t[i] = q[i] ^ r1;
        rest[i] = r1 ^ col_rot2(q[i]) ^ col_rot3(q[i]);
    }
    q[0] = t[7] ^ rest[0];
    q[1] = t[0] ^ t[7] ^ rest[1];
    q[2] = t[1] ^ rest[2];
    q[3] = t[2] ^ t[7] ^ rest[3];
    q[4] = t[3] ^ t[7] ^ rest[4];
    q[5] = t[4] ^ rest[5];
    q[6] = t[5] ^ rest[6];
    q[7] = t[6] ^ rest[7];
}

static void add_round_key(uint32_t q[8], const uint32_t rk[8]) {
    for (int i = 0; i < 8; i++) q[i] ^= rk[i];
}

void aes128_encrypt2(const aesgcm_t* k, const uint8_t in[32], uint8_t out[32]) {
    uint32_t q[8];
    load(q, in);
    add_round_key(q, k->rk[0]);
    for (int r = 1; r < 10; r++) {
        sub_bytes(q);
        shift_rows(q);
        mix_columns(q);
        add_round_key(q, k->rk[r]);
    }
    sub_bytes(q);
    shift_rows(q);
    add_round_key(q, k->rk[10]);
    store(q, out);
    ct_wipe(q, sizeof q);
}

// ---- The key schedule (FIPS 197, 5.2) ----

// SubWord: the S-box on the four bytes of a word, through the same circuit.
static uint32_t sub_word(uint32_t w) {
    uint32_t q[8], out = 0;
    for (int i = 0; i < 8; i++) q[i] = gather(w, i);
    sub_bytes(q);
    for (int i = 0; i < 8; i++) out |= scatter(q[i], i);
    ct_wipe(q, sizeof q);
    return out;
}

static void ghash_key(aesgcm_t* k);

void aesgcm_init(aesgcm_t* k, const uint8_t key[AESGCM_KEY_LEN]) {
    uint32_t w[44];  // little-endian: byte 0 of a word is its low byte
    uint8_t rcon = 1;
    for (int i = 0; i < 4; i++) w[i] = le32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = sub_word(t >> 8 | t << 24) ^ rcon;  // RotWord, SubWord, Rcon
            rcon = (uint8_t)(rcon << 1 ^ (rcon >> 7) * 0x1b);
        }
        w[i] = w[i - 4] ^ t;
    }
    // Each round key bitsliced, the same for both blocks.
    for (int r = 0; r < 11; r++) {
        for (int i = 0; i < 8; i++) {
            uint32_t x = 0;
            for (int c = 0; c < 4; c++) x |= gather(w[4 * r + c], i) << (4 * c);
            k->rk[r][i] = x | x << 16;
        }
    }
    ct_wipe(w, sizeof w);
    ghash_key(k);
}

// ---- GHASH (SP 800-38D, 6.4) ----

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void ghash_key(aesgcm_t* k) {
    uint8_t zero[32] = { 0 }, h[32];
    aes128_encrypt2(k, zero, h);
    for (int i = 0; i < 4; i++) k->h[i] = be32(h + 4 * i);
    ct_wipe(h, sizeof h);
}

// y = y * h in GCM's field, where the first bit of the block is the
// lowest power of x (so shifting "right" multiplies by x), reduced by
// x^128 + x^7 + x^2 + x + 1. A bit at a time, every step the same work.
static void gf_mul(uint32_t y[4], const uint32_t h[4]) {
    uint32_t z[4] = { 0, 0, 0, 0 }, v[4] = { h[0], h[1], h[2], h[3] };
    for (int i = 0; i < 128; i++) {
        uint32_t m = 0u - ((y[i >> 5] >> (31 - (i & 31))) & 1);
        z[0] ^= v[0] & m, z[1] ^= v[1] & m, z[2] ^= v[2] & m, z[3] ^= v[3] & m;
        uint32_t carry = 0u - (v[3] & 1);
        v[3] = v[3] >> 1 | v[2] << 31;
        v[2] = v[2] >> 1 | v[1] << 31;
        v[1] = v[1] >> 1 | v[0] << 31;
        v[0] = (v[0] >> 1) ^ (0xE1000000 & carry);
    }
    memcpy(y, z, sizeof z);
}

// Folds data, padded with zeros to whole blocks, into y.
static void ghash(const aesgcm_t* k, uint32_t y[4], const uint8_t* p, size_t len) {
    while (len) {
        uint8_t b[16] = { 0 };
        size_t n = len < 16 ? len : 16;
        memcpy(b, p, n);
        for (int i = 0; i < 4; i++) y[i] ^= be32(b + 4 * i);
        gf_mul(y, k->h);
        p += n;
        len -= n;
    }
}

// The tag: GHASH of aad, cipher text and their lengths in bits, masked
// with the encrypted first counter block.
static void make_tag(const aesgcm_t* k, const uint8_t ej0[16], const uint8_t* aad, size_t aadlen,
                     const uint8_t* cipher, size_t len, uint8_t tag[16]) {
    uint32_t y[4] = { 0, 0, 0, 0 };
    uint8_t lens[16];
    ghash(k, y, aad, aadlen);
    ghash(k, y, cipher, len);
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)len * 8;
    put_be32(lens, (uint32_t)(abits >> 32));
    put_be32(lens + 4, (uint32_t)abits);
    put_be32(lens + 8, (uint32_t)(cbits >> 32));
    put_be32(lens + 12, (uint32_t)cbits);
    ghash(k, y, lens, 16);
    for (int i = 0; i < 4; i++) put_be32(tag + 4 * i, y[i] ^ be32(ej0 + 4 * i));
}

// ---- Counter mode ----

static void counter_pair(uint8_t ctr[32], const uint8_t nonce[12], uint32_t n) {
    memcpy(ctr, nonce, 12);
    put_be32(ctr + 12, n);
    memcpy(ctr + 16, nonce, 12);
    put_be32(ctr + 28, n + 1);
}

// The first pair of counter blocks: E(J0) for the tag, and the first block of key stream.
static void first_pair(const aesgcm_t* k, const uint8_t nonce[12], uint8_t ks[32]) {
    uint8_t ctr[32];
    counter_pair(ctr, nonce, 1);
    aes128_encrypt2(k, ctr, ks);
}

// XORs the key stream into len bytes, starting with ks1 (counter 2), then counters 3 and 4, and so on.
static void ctr_xor(const aesgcm_t* k, const uint8_t nonce[12], const uint8_t ks1[16], const uint8_t* in,
                    size_t len, uint8_t* out) {
    uint8_t ctr[32], ks[32];
    size_t n = len < 16 ? len : 16;
    for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks1[i];
    in += n, out += n, len -= n;
    for (uint32_t c = 3; len; c += 2) {
        counter_pair(ctr, nonce, c);
        aes128_encrypt2(k, ctr, ks);
        n = len < 32 ? len : 32;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n, out += n, len -= n;
    }
    ct_wipe(ks, sizeof ks);
}

void aesgcm_seal(const aesgcm_t* k, const uint8_t nonce[AESGCM_NONCE_LEN], const void* aad, size_t aadlen,
                 const uint8_t* plain, size_t len, uint8_t* cipher, uint8_t tag[AESGCM_TAG_LEN]) {
    uint8_t ks[32];
    first_pair(k, nonce, ks);
    ctr_xor(k, nonce, ks + 16, plain, len, cipher);
    make_tag(k, ks, aad, aadlen, cipher, len, tag);
    ct_wipe(ks, sizeof ks);
}

int aesgcm_open(const aesgcm_t* k, const uint8_t nonce[AESGCM_NONCE_LEN], const void* aad, size_t aadlen,
                const uint8_t* cipher, size_t len, const uint8_t tag[AESGCM_TAG_LEN], uint8_t* plain) {
    uint8_t ks[32], want[16];
    first_pair(k, nonce, ks);
    make_tag(k, ks, aad, aadlen, cipher, len, want);
    int ok = ct_equal(want, tag, 16);
    if (ok) ctr_xor(k, nonce, ks + 16, cipher, len, plain);
    ct_wipe(ks, sizeof ks);
    return ok ? 0 : -1;
}

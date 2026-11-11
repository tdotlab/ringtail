// ChaCha20: see chacha20.h.
#include "chacha20.h"
#include "ct.h"
#include "lib.h"

static inline uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }
static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define QR(a, b, c, d)             \
    a += b; d = rotl(d ^ a, 16);   \
    c += d; b = rotl(b ^ c, 12);   \
    a += b; d = rotl(d ^ a, 8);    \
    c += d; b = rotl(b ^ c, 7)

// One 64-byte block of key stream from the 16-word state.
static void block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {  // 20 rounds: columns, then diagonals
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[4 * i] = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
    ct_wipe(x, sizeof x);
}

void chacha20_xor(const uint8_t key[CHACHA20_KEY_LEN], uint32_t counter, const uint8_t nonce[CHACHA20_NONCE_LEN],
                  const uint8_t* in, uint8_t* out, size_t len) {
    uint32_t st[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };  // "expand 32-byte k"
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13 + i] = le32(nonce + 4 * i);
    uint8_t ks[64];
    while (len) {
        block(st, ks);
        st[12]++;
        size_t n = len < 64 ? len : 64;
        if (in) {
            for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
            in += n;
        } else {
            memcpy(out, ks, n);
        }
        out += n;
        len -= n;
    }
    ct_wipe(ks, sizeof ks);
    ct_wipe(st, sizeof st);
}

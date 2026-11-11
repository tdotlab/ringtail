// RSA and ECDSA signature checking: see sig.h.
#include "sig.h"
#include "sha256.h"
#include "sha512.h"
#include "der.h"
#include "lib.h"

uint32_t hash_size(int hash) {
    return hash == HASH_SHA256 ? 32 : hash == HASH_SHA384 ? 48 : hash == HASH_SHA512 ? 64 : 0;
}

void hash_once(int hash, const void* data, size_t len, uint8_t* out) {
    if (hash == HASH_SHA256) sha256(data, len, out);
    else if (hash == HASH_SHA384) sha384(data, len, out);
    else if (hash == HASH_SHA512) sha512(data, len, out);
}

// ---- Big numbers ----
//
// Numbers are arrays of 32-bit limbs, least significant first, all of
// one length for any one modulus. Arithmetic modulo m is done in
// Montgomery form: a number a is kept as aR mod m (R = 2^(32 * limbs)),
// which turns the division a product would need into shifts.

#define LIMBS 128  // 4096 bits
typedef uint32_t limb_t;

typedef struct {
    uint32_t n;         // limbs
    limb_t m[LIMBS];    // the modulus, odd
    limb_t m0;          // -1/m mod 2^32
    limb_t rr[LIMBS];   // R^2 mod m, for moving into Montgomery form
    limb_t one[LIMBS];  // R mod m: 1, in Montgomery form
} mont_t;

static int cmp(const limb_t* a, const limb_t* b, uint32_t n) {
    for (uint32_t i = n; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int is_zero(const limb_t* a, uint32_t n) {
    limb_t x = 0;
    for (uint32_t i = 0; i < n; i++) x |= a[i];
    return x == 0;
}

static limb_t add(limb_t* r, const limb_t* a, const limb_t* b, uint32_t n) {
    uint64_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (limb_t)c;
        c >>= 32;
    }
    return (limb_t)c;
}

static limb_t sub(limb_t* r, const limb_t* a, const limb_t* b, uint32_t n) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (limb_t)d;
        borrow = d >> 63;
    }
    return (limb_t)borrow;
}

// Big-endian bytes (leading zeros allowed) into n limbs; -1 if the
// number does not fit.
static int load(limb_t* r, uint32_t n, const uint8_t* b, uint32_t len) {
    while (len && !*b) {
        b++;
        len--;
    }
    if (len > 4 * n) return -1;
    memset(r, 0, 4 * n);
    for (uint32_t i = 0; i < len; i++) r[i / 4] |= (limb_t)b[len - 1 - i] << (8 * (i % 4));
    return 0;
}

static void store(uint8_t* b, uint32_t len, const limb_t* a) {
    for (uint32_t i = 0; i < len; i++) b[len - 1 - i] = (uint8_t)(a[i / 4] >> (8 * (i % 4)));
}

// a * b / R mod m, for a and b below m (CIOS: Koc, Acar and Kaliski,
// "Analyzing and comparing Montgomery multiplication algorithms", 1996).
// r may be a or b.
static void mont_mul(const mont_t* M, limb_t* r, const limb_t* a, const limb_t* b) {
    uint32_t n = M->n;
    limb_t t[LIMBS + 2];
    memset(t, 0, 4 * (n + 2));
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = 0;
        for (uint32_t j = 0; j < n; j++) {
            c += (uint64_t)a[j] * b[i] + t[j];
            t[j] = (limb_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n] = (limb_t)c;
        t[n + 1] = (limb_t)(c >> 32);
        // Add the multiple of m that clears the lowest limb, and drop it.
        limb_t u = t[0] * M->m0;
        c = ((uint64_t)u * M->m[0] + t[0]) >> 32;
        for (uint32_t j = 1; j < n; j++) {
            c += (uint64_t)u * M->m[j] + t[j];
            t[j - 1] = (limb_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n - 1] = (limb_t)c;
        t[n] = t[n + 1] + (limb_t)(c >> 32);
    }
    // Now t < 2m.
    if (t[n] || cmp(t, M->m, n) >= 0) sub(r, t, M->m, n);
    else memcpy(r, t, 4 * n);
}

static void to_mont(const mont_t* M, limb_t* r, const limb_t* a) { mont_mul(M, r, a, M->rr); }

static void from_mont(const mont_t* M, limb_t* r, const limb_t* a) {
    limb_t one[LIMBS];
    memset(one, 0, 4 * M->n);
    one[0] = 1;
    mont_mul(M, r, a, one);
}

// base^e, both in and out in Montgomery form; e as big-endian bytes.
static void mont_exp(const mont_t* M, limb_t* r, const limb_t* base, const uint8_t* e, uint32_t e_len) {
    limb_t acc[LIMBS], b[LIMBS];
    memcpy(b, base, 4 * M->n);
    memcpy(acc, M->one, 4 * M->n);
    for (uint32_t i = 0; i < e_len; i++)
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(M, acc, acc, acc);
            if (e[i] >> bit & 1) mont_mul(M, acc, acc, b);
        }
    memcpy(r, acc, 4 * M->n);
}

// Sets up arithmetic modulo m (big-endian bytes): -1 if m is even, 1,
// or longer than 4096 bits.
static int mont_init(mont_t* M, const uint8_t* m, uint32_t len) {
    while (len && !*m) {
        m++;
        len--;
    }
    if (!len || len > 4 * LIMBS || !(m[len - 1] & 1) || (len == 1 && m[0] == 1)) return -1;
    M->n = (len + 3) / 4;
    load(M->m, M->n, m, len);
    // Newton's iteration doubles the correct low bits of 1/m each step;
    // m itself is right to 3 bits (an odd square is 1 mod 8).
    limb_t x = M->m[0];
    for (int i = 0; i < 5; i++) x *= 2 - M->m[0] * x;
    M->m0 = -x;
    // R mod m, by doubling 1 (mod m) as often as R has bits; once more
    // is 2 in Montgomery form, and that to the power of R's bits is
    // 2^(bits) = R, in Montgomery form: R^2 mod m.
    limb_t v[LIMBS];
    memset(v, 0, sizeof v);
    v[0] = 1;
    for (uint32_t i = 0; i <= 32 * M->n; i++) {
        if (i == 32 * M->n) memcpy(M->one, v, 4 * M->n);
        limb_t carry = add(v, v, v, M->n);
        if (carry || cmp(v, M->m, M->n) >= 0) sub(v, v, M->m, M->n);
    }
    uint8_t bits[2] = { (uint8_t)(32 * M->n >> 8), (uint8_t)(32 * M->n) };
    mont_exp(M, M->rr, v, bits, 2);
    return 0;
}

static void mod_add(const mont_t* M, limb_t* r, const limb_t* a, const limb_t* b) {
    if (add(r, a, b, M->n) || cmp(r, M->m, M->n) >= 0) sub(r, r, M->m, M->n);
}

static void mod_sub(const mont_t* M, limb_t* r, const limb_t* a, const limb_t* b) {
    if (sub(r, a, b, M->n)) add(r, r, M->m, M->n);
}

// ---- RSA ----

// s^e mod n, as k bytes (k: the modulus's length in bytes), after the
// checks every RSA signature must pass. *bits: the modulus's length in bits.
static int rsa_open(const uint8_t* n, uint32_t n_len, const uint8_t* e, uint32_t e_len, const uint8_t* sig,
                    uint32_t sig_len, uint8_t* em, uint32_t* k, uint32_t* bits) {
    while (n_len && !*n) {
        n++;
        n_len--;
    }
    while (e_len && !*e) {
        e++;
        e_len--;
    }
    if (n_len > 4 * LIMBS) return SIG_UNSUPPORTED;
    // A key with an even modulus, or an exponent of 0, 1 or an even one, is not an RSA key.
    if (n_len < 64 || !(n[n_len - 1] & 1) || !e_len || !(e[e_len - 1] & 1) || (e_len == 1 && e[0] == 1) ||
        e_len > n_len)
        return SIG_BAD;
    if (sig_len != n_len) return SIG_BAD;  // the signature is exactly as long as the modulus (8.2.2)
    mont_t M;
    if (mont_init(&M, n, n_len) != 0) return SIG_BAD;
    limb_t s[LIMBS];
    load(s, M.n, sig, sig_len);
    if (cmp(s, M.m, M.n) >= 0) return SIG_BAD;  // the signature is a number below the modulus (5.2.2)
    to_mont(&M, s, s);
    mont_exp(&M, s, s, e, e_len);
    from_mont(&M, s, s);
    store(em, n_len, s);
    *k = n_len;
    *bits = 8 * n_len;
    for (uint8_t top = n[0]; !(top & 0x80); top <<= 1) (*bits)--;
    return SIG_OK;
}

// The DER of DigestInfo up to the hash itself (RFC 8017 9.2, note 1).
static const uint8_t digest_info[3][19] = {
    { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 },
    { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 },
    { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 },
};

int rsa_pkcs1_verify(const uint8_t* n, uint32_t n_len, const uint8_t* e, uint32_t e_len, int hash, const void* msg,
                     size_t msg_len, const uint8_t* sig, uint32_t sig_len) {
    uint32_t h_len = hash_size(hash), k, bits;
    if (!h_len) return SIG_UNSUPPORTED;
    uint8_t em[4 * LIMBS], want[4 * LIMBS];
    int r = rsa_open(n, n_len, e, e_len, sig, sig_len, em, &k, &bits);
    if (r) return r;
    // Rather than take apart what came out, build what should have and
    // compare (8.2.2): 00 01 FF..FF 00 DigestInfo hash.
    uint32_t t_len = 19 + h_len;
    if (k < t_len + 11) return SIG_BAD;
    want[0] = 0;
    want[1] = 1;
    memset(want + 2, 0xff, k - t_len - 3);
    want[k - t_len - 1] = 0;
    memcpy(want + k - t_len, digest_info[hash - HASH_SHA256], 19);
    hash_once(hash, msg, msg_len, want + k - h_len);
    return memcmp(em, want, k) == 0 ? SIG_OK : SIG_BAD;
}

// MGF1 (B.2.1): mask bytes from a seed, XORed into out.
static void mgf1_xor(int hash, const uint8_t* seed, uint32_t seed_len, uint8_t* out, uint32_t len) {
    uint8_t in[HASH_MAX_LEN + 4], block[HASH_MAX_LEN];
    uint32_t h_len = hash_size(hash);
    memcpy(in, seed, seed_len);
    for (uint32_t counter = 0, at = 0; at < len; counter++) {
        in[seed_len] = (uint8_t)(counter >> 24);
        in[seed_len + 1] = (uint8_t)(counter >> 16);
        in[seed_len + 2] = (uint8_t)(counter >> 8);
        in[seed_len + 3] = (uint8_t)counter;
        hash_once(hash, in, seed_len + 4, block);
        for (uint32_t i = 0; i < h_len && at < len; i++) out[at++] ^= block[i];
    }
}

int rsa_pss_verify(const uint8_t* n, uint32_t n_len, const uint8_t* e, uint32_t e_len, int hash, uint32_t salt_len,
                   const void* msg, size_t msg_len, const uint8_t* sig, uint32_t sig_len) {
    uint32_t h_len = hash_size(hash), k, bits;
    if (!h_len || salt_len > HASH_MAX_LEN) return SIG_UNSUPPORTED;
    uint8_t em[4 * LIMBS];
    int r = rsa_open(n, n_len, e, e_len, sig, sig_len, em, &k, &bits);
    if (r) return r;
    // EM is emBits = bits - 1 long (8.1.2, 9.1.2); when that is a whole
    // number of bytes, the top byte of what came out must be 0.
    uint32_t em_bits = bits - 1, em_len = (em_bits + 7) / 8;
    uint8_t* p = em;
    if (em_len < k) {
        if (em[0]) return SIG_BAD;
        p++;
    }
    if (em_len < h_len + salt_len + 2 || p[em_len - 1] != 0xbc) return SIG_BAD;
    uint32_t db_len = em_len - h_len - 1;
    uint8_t* db = p;
    const uint8_t* h = p + db_len;
    uint8_t top_mask = (uint8_t)(0xff << (8 - (8 * em_len - em_bits)));  // the bits above emBits
    if (8 * em_len - em_bits && (db[0] & top_mask)) return SIG_BAD;
    mgf1_xor(hash, h, h_len, db, db_len);
    if (8 * em_len - em_bits) db[0] &= (uint8_t)~top_mask;
    uint32_t zeros = db_len - salt_len - 1;
    for (uint32_t i = 0; i < zeros; i++)
        if (db[i]) return SIG_BAD;
    if (db[zeros] != 1) return SIG_BAD;
    // H' = Hash(8 zero bytes, Hash(M), salt), which must be H.
    uint8_t m2[8 + 2 * HASH_MAX_LEN], h2[HASH_MAX_LEN];
    memset(m2, 0, 8);
    hash_once(hash, msg, msg_len, m2 + 8);
    memcpy(m2 + 8 + h_len, db + db_len - salt_len, salt_len);
    hash_once(hash, m2, 8 + h_len + salt_len, h2);
    return memcmp(h, h2, h_len) == 0 ? SIG_OK : SIG_BAD;
}

// ---- ECDSA ----

typedef struct {
    uint32_t size;  // bytes in each number
    const uint8_t *p, *n, *b, *gx, *gy;  // big-endian; the curve is y^2 = x^3 - 3x + b mod p
} curve_t;

static const uint8_t p256_p[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t p256_n[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
static const uint8_t p256_b[32] = {
    0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
    0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b,
};
static const uint8_t p256_gx[32] = {
    0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
    0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
static const uint8_t p256_gy[32] = {
    0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
    0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};
static const uint8_t p384_p[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t p384_n[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf,
    0x58, 0x1a, 0x0d, 0xb2, 0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73,
};
static const uint8_t p384_b[48] = {
    0xb3, 0x31, 0x2f, 0xa7, 0xe2, 0x3e, 0xe7, 0xe4, 0x98, 0x8e, 0x05, 0x6b, 0xe3, 0xf8, 0x2d, 0x19,
    0x18, 0x1d, 0x9c, 0x6e, 0xfe, 0x81, 0x41, 0x12, 0x03, 0x14, 0x08, 0x8f, 0x50, 0x13, 0x87, 0x5a,
    0xc6, 0x56, 0x39, 0x8d, 0x8a, 0x2e, 0xd1, 0x9d, 0x2a, 0x85, 0xc8, 0xed, 0xd3, 0xec, 0x2a, 0xef,
};
static const uint8_t p384_gx[48] = {
    0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37, 0x8e, 0xb1, 0xc7, 0x1e, 0xf3, 0x20, 0xad, 0x74,
    0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98, 0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38,
    0x55, 0x02, 0xf2, 0x5d, 0xbf, 0x55, 0x29, 0x6c, 0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7,
};
static const uint8_t p384_gy[48] = {
    0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f, 0x5d, 0x9e, 0x98, 0xbf, 0x92, 0x92, 0xdc, 0x29,
    0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c, 0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0,
    0x0a, 0x60, 0xb1, 0xce, 0x1d, 0x7e, 0x81, 0x9d, 0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f,
};

static const curve_t curves[] = {
    { 32, p256_p, p256_n, p256_b, p256_gx, p256_gy },
    { 48, p384_p, p384_n, p384_b, p384_gx, p384_gy },
};

uint32_t curve_size(int curve) { return curve == CURVE_P256 || curve == CURVE_P384 ? curves[curve - 1].size : 0; }

#define EC_LIMBS 12  // 384 bits

// A point in Jacobian coordinates (x/z^2, y/z^3), each in Montgomery
// form; z = 0 is the point at infinity.
typedef struct {
    limb_t x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS];
} point_t;

// 2P, for a = -3 ("dbl-2001-b", Bernstein and Lange's Explicit-Formulas Database).
static void point_double(const mont_t* F, point_t* r, const point_t* p) {
    uint32_t n = F->n;
    if (is_zero(p->z, n) || is_zero(p->y, n)) {
        memset(r, 0, sizeof *r);
        return;
    }
    limb_t delta[EC_LIMBS], gamma[EC_LIMBS], beta[EC_LIMBS], alpha[EC_LIMBS], t[EC_LIMBS], u[EC_LIMBS];
    mont_mul(F, delta, p->z, p->z);
    mont_mul(F, gamma, p->y, p->y);
    mont_mul(F, beta, p->x, gamma);
    mod_sub(F, t, p->x, delta);
    mod_add(F, u, p->x, delta);
    mont_mul(F, alpha, t, u);
    mod_add(F, t, alpha, alpha);
    mod_add(F, alpha, t, alpha);  // alpha = 3 (x - delta)(x + delta)
    point_t q;
    // z3 = (y + z)^2 - gamma - delta
    mod_add(F, t, p->y, p->z);
    mont_mul(F, t, t, t);
    mod_sub(F, t, t, gamma);
    mod_sub(F, q.z, t, delta);
    // x3 = alpha^2 - 8 beta
    mod_add(F, beta, beta, beta);
    mod_add(F, beta, beta, beta);  // 4 beta
    mod_add(F, u, beta, beta);     // 8 beta
    mont_mul(F, t, alpha, alpha);
    mod_sub(F, q.x, t, u);
    // y3 = alpha (4 beta - x3) - 8 gamma^2
    mod_sub(F, t, beta, q.x);
    mont_mul(F, t, alpha, t);
    mont_mul(F, u, gamma, gamma);
    mod_add(F, u, u, u);
    mod_add(F, u, u, u);
    mod_add(F, u, u, u);
    mod_sub(F, q.y, t, u);
    *r = q;
}

// P + Q ("add-1998-cmo-2"), with the cases the formula does not cover:
// either at infinity, Q = P (a doubling), and Q = -P.
static void point_add(const mont_t* F, point_t* r, const point_t* p, const point_t* q) {
    uint32_t n = F->n;
    if (is_zero(p->z, n)) {
        *r = *q;
        return;
    }
    if (is_zero(q->z, n)) {
        *r = *p;
        return;
    }
    limb_t z1z1[EC_LIMBS], z2z2[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], s1[EC_LIMBS], s2[EC_LIMBS];
    limb_t h[EC_LIMBS], rr[EC_LIMBS], hh[EC_LIMBS], hhh[EC_LIMBS], v[EC_LIMBS], t[EC_LIMBS];
    mont_mul(F, z1z1, p->z, p->z);
    mont_mul(F, z2z2, q->z, q->z);
    mont_mul(F, u1, p->x, z2z2);
    mont_mul(F, u2, q->x, z1z1);
    mont_mul(F, s1, p->y, q->z);
    mont_mul(F, s1, s1, z2z2);
    mont_mul(F, s2, q->y, p->z);
    mont_mul(F, s2, s2, z1z1);
    mod_sub(F, h, u2, u1);
    mod_sub(F, rr, s2, s1);
    if (is_zero(h, n)) {
        if (is_zero(rr, n)) point_double(F, r, p);
        else memset(r, 0, sizeof *r);
        return;
    }
    point_t out;
    mont_mul(F, hh, h, h);
    mont_mul(F, hhh, h, hh);
    mont_mul(F, v, u1, hh);
    // x3 = r^2 - h^3 - 2v
    mont_mul(F, t, rr, rr);
    mod_sub(F, t, t, hhh);
    mod_sub(F, t, t, v);
    mod_sub(F, out.x, t, v);
    // y3 = r (v - x3) - s1 h^3
    mod_sub(F, t, v, out.x);
    mont_mul(F, t, rr, t);
    mont_mul(F, s1, s1, hhh);
    mod_sub(F, out.y, t, s1);
    // z3 = z1 z2 h
    mont_mul(F, t, p->z, q->z);
    mont_mul(F, out.z, t, h);
    *r = out;
}

// Bytes minus a small number (the modulus less 2, for inverting by Fermat).
static void minus(uint8_t* out, const uint8_t* in, uint32_t len, uint8_t k) {
    int borrow = k;
    for (uint32_t i = len; i-- > 0;) {
        int v = in[i] - borrow;
        borrow = v < 0;
        out[i] = (uint8_t)(v + (borrow ? 256 : 0));
    }
}

static int bit(const limb_t* a, uint32_t i) { return a[i / 32] >> (i % 32) & 1; }

int ecdsa_verify(int curve, const uint8_t* point, uint32_t point_len, int hash, const void* msg, size_t msg_len,
                 const uint8_t* sig, uint32_t sig_len) {
    if (!curve_size(curve) || !hash_size(hash)) return SIG_UNSUPPORTED;
    const curve_t* C = &curves[curve - 1];
    uint32_t size = C->size, n = size / 4;
    if (point_len == 1 + size && (point[0] == 2 || point[0] == 3)) return SIG_UNSUPPORTED;  // compressed
    if (point_len != 1 + 2 * size || point[0] != 4) return SIG_BAD;

    // The signature: two numbers in [1, n-1], and nothing else.
    der_t d = der_of(sig, sig_len), seq;
    const uint8_t *r_bytes, *s_bytes;
    uint32_t r_len, s_len;
    der_expect(&d, DER_SEQUENCE, &seq);
    der_uint(&seq, &r_bytes, &r_len);
    der_uint(&seq, &s_bytes, &s_len);
    if (d.bad || d.n || seq.bad || seq.n) return SIG_BAD;
    mont_t F, N;
    mont_init(&F, C->p, size);
    mont_init(&N, C->n, size);
    limb_t r[EC_LIMBS], s[EC_LIMBS], e[EC_LIMBS];
    if (load(r, n, r_bytes, r_len) || load(s, n, s_bytes, s_len)) return SIG_BAD;
    if (is_zero(r, n) || is_zero(s, n) || cmp(r, N.m, n) >= 0 || cmp(s, N.m, n) >= 0) return SIG_BAD;

    // The key: a point on the curve (its coordinates below p).
    point_t q, g, gq, acc;
    limb_t t[EC_LIMBS], u[EC_LIMBS];
    if (load(q.x, n, point + 1, size) || load(q.y, n, point + 1 + size, size)) return SIG_BAD;
    if (cmp(q.x, F.m, n) >= 0 || cmp(q.y, F.m, n) >= 0) return SIG_BAD;
    to_mont(&F, q.x, q.x);
    to_mont(&F, q.y, q.y);
    memcpy(q.z, F.one, 4 * n);
    load(t, n, C->b, size);
    to_mont(&F, t, t);  // b
    mont_mul(&F, u, q.x, q.x);
    mont_mul(&F, u, u, q.x);  // x^3
    mod_add(&F, u, u, t);
    mod_sub(&F, u, u, q.x);
    mod_sub(&F, u, u, q.x);
    mod_sub(&F, u, u, q.x);  // x^3 - 3x + b
    mont_mul(&F, t, q.y, q.y);
    if (cmp(t, u, n) != 0) return SIG_BAD;

    // e: the hash's leftmost bits, as many as n has (the curves' orders
    // fill their bytes), reduced mod n; it is below 2n, so one
    // subtraction does.
    uint8_t h[HASH_MAX_LEN];
    uint32_t h_len = hash_size(hash);
    hash_once(hash, msg, msg_len, h);
    load(e, n, h, h_len < size ? h_len : size);
    if (cmp(e, N.m, n) >= 0) sub(e, e, N.m, n);

    // u1 = e / s, u2 = r / s (mod n), 1/s being s^(n-2).
    uint8_t exp[48];
    limb_t w[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS];
    minus(exp, C->n, size, 2);
    to_mont(&N, w, s);
    mont_exp(&N, w, w, exp, size);
    to_mont(&N, u1, e);
    mont_mul(&N, u1, u1, w);
    from_mont(&N, u1, u1);
    to_mont(&N, u2, r);
    mont_mul(&N, u2, u2, w);
    from_mont(&N, u2, u2);

    // u1 G + u2 Q, both at once (Shamir's trick): one doubling a bit,
    // and an addition of G, Q or G + Q.
    load(g.x, n, C->gx, size);
    load(g.y, n, C->gy, size);
    to_mont(&F, g.x, g.x);
    to_mont(&F, g.y, g.y);
    memcpy(g.z, F.one, 4 * n);
    point_add(&F, &gq, &g, &q);
    memset(&acc, 0, sizeof acc);
    for (uint32_t i = 8 * size; i-- > 0;) {
        point_double(&F, &acc, &acc);
        int which = bit(u1, i) | bit(u2, i) << 1;
        if (which) point_add(&F, &acc, &acc, which == 1 ? &g : which == 2 ? &q : &gq);
    }
    if (is_zero(acc.z, n)) return SIG_BAD;

    // Its x, x/z^2, mod n, must be r.
    minus(exp, C->p, size, 2);
    mont_exp(&F, t, acc.z, exp, size);  // 1/z
    mont_mul(&F, t, t, t);
    mont_mul(&F, t, acc.x, t);
    from_mont(&F, t, t);
    if (cmp(t, N.m, n) >= 0) sub(t, t, N.m, n);
    return cmp(t, r, n) == 0 ? SIG_OK : SIG_BAD;
}

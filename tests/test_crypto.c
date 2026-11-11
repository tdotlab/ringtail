// Host test for the cryptography TLS stands on: SHA-256, HMAC and HKDF
// (src/lib/sha256.c), ChaCha20 (src/lib/chacha20.c), Poly1305 and the
// two together (src/programs/lib/chachapoly.c), and X25519
// (src/programs/lib/x25519.c). First the worked examples from the
// documents that define them, then crypto_cases.h: Project Wycheproof's
// vectors, built to catch the mistakes implementations really make, and
// OpenSSL's answers for inputs of every awkward length
// (tests/gen_crypto_cases.py makes it). Run under the address and
// undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "chacha20.h"
#include "chachapoly.h"
#include "x25519.h"
#include "ct.h"
#include "crypto_cases.h"

static int failures, checks;

static void check(int ok, const char* what, unsigned idx) {
    checks++;
    if (ok) return;
    failures++;
    if (failures <= 20) printf("FAIL: %s (case %u)\n", what, idx);
}

// Hex to bytes in a buffer of exactly that size, so the sanitizer
// catches a read one past the end.
typedef struct { uint8_t* p; size_t n; } bytes_t;

static bytes_t hex(const char* s) {
    bytes_t b;
    b.n = strlen(s) / 2;
    b.p = malloc(b.n ? b.n : 1);
    for (size_t i = 0; i < b.n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        b.p[i] = (uint8_t)v;
    }
    return b;
}

static int eq(const uint8_t* got, const char* want_hex) {
    bytes_t w = hex(want_hex);
    int ok = w.n == 0 || memcmp(got, w.p, w.n) == 0;
    free(w.p);
    return ok;
}

#define N(a) (sizeof(a) / sizeof *(a))

static void test_sha256(void) {
    uint8_t d[32];
    sha256("abc", 3, d);
    check(eq(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc", 0);
    sha256("", 0, d);
    check(eq(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing", 0);
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d);
    check(eq(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 of the two-block message", 0);
    sha256_t s;
    sha256_init(&s);
    char as[1000];
    memset(as, 'a', sizeof as);
    for (int i = 0; i < 1000; i++) sha256_update(&s, as, sizeof as);
    sha256_final(&s, d);
    check(eq(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million a's", 0);

    for (unsigned i = 0; i < N(sha256_cases); i++) {
        bytes_t m = hex(sha256_cases[i].f[0]);
        sha256(m.p, m.n, d);
        check(eq(d, sha256_cases[i].f[1]), "SHA-256 matches OpenSSL", i);
        // The same bytes fed in two pieces, cut at every place (or a spread of them).
        size_t step = m.n > 300 ? m.n / 97 : 1;
        int same = 1;
        for (size_t cut = 0; cut <= m.n; cut += step) {
            uint8_t d2[32];
            sha256_init(&s);
            sha256_update(&s, m.p, cut);
            sha256_update(&s, m.p + cut, m.n - cut);
            sha256_final(&s, d2);
            same &= memcmp(d, d2, 32) == 0;
        }
        check(same, "SHA-256 fed in two pieces", i);
        free(m.p);
    }
}

static void test_hmac_hkdf(void) {
    uint8_t d[32], key[20];
    memset(key, 0x0b, sizeof key);
    hmac_sha256(key, 20, "Hi There", 8, d);
    check(eq(d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC, RFC 4231 case 1", 0);
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    check(eq(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "HMAC, RFC 4231 case 2", 0);
    for (unsigned i = 0; i < N(hmac_cases); i++) {
        bytes_t k = hex(hmac_cases[i].f[0]), m = hex(hmac_cases[i].f[1]), t = hex(hmac_cases[i].f[2]);
        hmac_sha256(k.p, k.n, m.p, m.n, d);
        int match = t.n == hmac_cases[i].n && memcmp(d, t.p, t.n) == 0;
        check(match == hmac_cases[i].ok, "HMAC (Wycheproof)", i);
        free(k.p); free(m.p); free(t.p);
    }

    uint8_t ikm[22], prk[32], okm[42];
    memset(ikm, 0x0b, sizeof ikm);
    bytes_t salt = hex("000102030405060708090a0b0c"), info = hex("f0f1f2f3f4f5f6f7f8f9");
    hkdf_sha256_extract(salt.p, salt.n, ikm, sizeof ikm, prk);
    check(eq(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"), "HKDF extract, RFC 5869 case 1", 0);
    check(hkdf_sha256_expand(prk, info.p, info.n, okm, sizeof okm) == 0 &&
          eq(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
          "HKDF expand, RFC 5869 case 1", 0);
    free(salt.p); free(info.p);
    hkdf_sha256_extract(0, 0, ikm, sizeof ikm, prk);
    check(eq(prk, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"), "HKDF extract with no salt, case 3", 0);
    check(hkdf_sha256_expand(prk, 0, 0, okm, sizeof okm) == 0 &&
          eq(okm, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
          "HKDF expand with no info, case 3", 0);
    for (unsigned i = 0; i < N(hkdf_cases); i++) {
        const crypto_case_t* c = &hkdf_cases[i];
        bytes_t k = hex(c->f[0]), s = hex(c->f[1]), in = hex(c->f[2]);
        uint8_t* out = malloc(c->n ? c->n : 1);
        hkdf_sha256_extract(s.p, s.n, k.p, k.n, prk);
        int r = hkdf_sha256_expand(prk, in.p, in.n, out, c->n);
        if (c->ok) check(r == 0 && eq(out, c->f[3]), "HKDF (Wycheproof)", i);
        else check(r == -1, "HKDF refuses an output that is too long", i);
        free(k.p); free(s.p); free(in.p); free(out);
    }
}

static void test_chacha20(void) {
    const char* sun = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                      "sunscreen would be it.";
    bytes_t key = hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    bytes_t nonce = hex("000000000000004a00000000");
    uint8_t out[114];
    chacha20_xor(key.p, 1, nonce.p, (const uint8_t*)sun, out, 114);
    check(eq(out, "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b357"
                  "1639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
                  "5af90bbf74a35be6b40b8eedf2785e42874d"), "ChaCha20, RFC 8439 2.4.2", 0);
    free(key.p); free(nonce.p);
    for (unsigned i = 0; i < N(chacha20_cases); i++) {
        const crypto_case_t* c = &chacha20_cases[i];
        bytes_t k = hex(c->f[0]), n = hex(c->f[1]), in = hex(c->f[2]);
        uint8_t* o = malloc(in.n ? in.n : 1);
        chacha20_xor(k.p, c->n, n.p, in.p, o, in.n);
        check(eq(o, c->f[3]), "ChaCha20 matches OpenSSL", i);
        chacha20_xor(k.p, c->n, n.p, o, o, in.n);  // in place, and back again
        check(in.n == 0 || memcmp(o, in.p, in.n) == 0, "ChaCha20 undoes itself in place", i);
        // The key stream alone is what encrypting zeros gives.
        uint8_t* z = calloc(1, in.n ? in.n : 1);
        chacha20_xor(k.p, c->n, n.p, 0, o, in.n);
        chacha20_xor(k.p, c->n, n.p, z, z, in.n);
        check(in.n == 0 || memcmp(o, z, in.n) == 0, "ChaCha20 key stream alone", i);
        free(k.p); free(n.p); free(in.p); free(o); free(z);
    }
}

static void test_poly1305(void) {
    bytes_t key = hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    uint8_t tag[16];
    poly1305_t p;
    poly1305_init(&p, key.p);
    poly1305_update(&p, "Cryptographic Forum Research Group", 34);
    poly1305_final(&p, tag);
    check(eq(tag, "a8061dc1305136c6c22b8baf0c0127a9"), "Poly1305, RFC 8439 2.5.2", 0);
    free(key.p);
    for (unsigned i = 0; i < N(poly1305_cases); i++) {
        const crypto_case_t* c = &poly1305_cases[i];
        bytes_t k = hex(c->f[0]), m = hex(c->f[1]);
        poly1305_init(&p, k.p);
        poly1305_update(&p, m.p, m.n);
        poly1305_final(&p, tag);
        check(eq(tag, c->f[2]), "Poly1305 matches OpenSSL", i);
        int same = 1;
        for (size_t cut = 0; cut <= m.n && cut < 80; cut++) {  // in three pieces
            uint8_t t2[16];
            poly1305_init(&p, k.p);
            poly1305_update(&p, m.p, cut / 2);
            poly1305_update(&p, m.p + cut / 2, cut - cut / 2);
            poly1305_update(&p, m.p + cut, m.n - cut);
            poly1305_final(&p, t2);
            same &= memcmp(tag, t2, 16) == 0;
        }
        check(same, "Poly1305 fed in pieces", i);
        free(k.p); free(m.p);
    }
}

static void test_aead(void) {
    for (unsigned i = 0; i < N(aead_cases); i++) {
        const crypto_case_t* c = &aead_cases[i];
        bytes_t k = hex(c->f[0]), n = hex(c->f[1]), aad = hex(c->f[2]), m = hex(c->f[3]), ct = hex(c->f[4]), tag = hex(c->f[5]);
        uint8_t* out = malloc(ct.n ? ct.n : 1);
        uint8_t t[16];
        memset(out, 0xEE, ct.n);
        int r = chachapoly_open(k.p, n.p, aad.p, aad.n, ct.p, ct.n, tag.p, out);
        if (c->ok) {
            check(r == 0 && (m.n == 0 || memcmp(out, m.p, m.n) == 0), "AEAD opens", i);
            chachapoly_seal(k.p, n.p, aad.p, aad.n, m.p, m.n, out, t);
            check((ct.n == 0 || memcmp(out, ct.p, ct.n) == 0) && memcmp(t, tag.p, 16) == 0, "AEAD seals", i);
            // In place, the way TLS uses it.
            check(chachapoly_open(k.p, n.p, aad.p, aad.n, out, ct.n, t, out) == 0 &&
                  (m.n == 0 || memcmp(out, m.p, m.n) == 0), "AEAD opens in place", i);
            // One changed bit anywhere must be noticed. (Every bit for short cases, a spread for long.)
            int caught = 1;
            size_t step = ct.n > 64 ? ct.n * 8 / 61 : 1;
            for (size_t bit = 0; bit < ct.n * 8; bit += step) {
                ct.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
                caught &= chachapoly_open(k.p, n.p, aad.p, aad.n, ct.p, ct.n, tag.p, out) == -1;
                ct.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
            }
            for (size_t bit = 0; bit < aad.n * 8; bit++) {
                aad.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
                caught &= chachapoly_open(k.p, n.p, aad.p, aad.n, ct.p, ct.n, tag.p, out) == -1;
                aad.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
            }
            for (size_t bit = 0; bit < 128; bit++) {
                tag.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
                caught &= chachapoly_open(k.p, n.p, aad.p, aad.n, ct.p, ct.n, tag.p, out) == -1;
                tag.p[bit / 8] ^= (uint8_t)(1 << (bit % 8));
            }
            check(caught, "AEAD notices any one changed bit", i);
        } else {
            int untouched = 1;
            for (size_t j = 0; j < ct.n; j++) untouched &= out[j] == 0xEE;
            check(r == -1 && untouched, "AEAD refuses a forgery and writes nothing", i);
        }
        free(k.p); free(n.p); free(aad.p); free(m.p); free(ct.p); free(tag.p); free(out);
    }
}

static void test_x25519(void) {
    uint8_t out[32];
    bytes_t k = hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    bytes_t u = hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    check(x25519(out, k.p, u.p) == 0 && eq(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
          "X25519, RFC 7748 first vector", 0);
    free(k.p); free(u.p);
    k = hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
    u = hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
    check(x25519(out, k.p, u.p) == 0 && eq(out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
          "X25519, RFC 7748 second vector", 0);
    free(k.p); free(u.p);

    // The RFC's loop: k and u start as 9; each round k becomes X25519(k, u) and u the old k.
    uint8_t kk[32] = { 9 }, uu[32] = { 9 };
    for (int i = 1; i <= 1000; i++) {
        x25519(out, kk, uu);
        memcpy(uu, kk, 32);
        memcpy(kk, out, 32);
        if (i == 1) check(eq(kk, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"), "X25519 after one round", 0);
    }
    check(eq(kk, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"), "X25519 after 1000 rounds", 0);

    // Alice and Bob, RFC 7748 6.1.
    bytes_t a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    bytes_t b = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    uint8_t pa[32], pb[32], s1[32], s2[32];
    x25519_public(pa, a.p);
    x25519_public(pb, b.p);
    check(eq(pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "Alice's public value", 0);
    check(eq(pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "Bob's public value", 0);
    check(x25519(s1, a.p, pb) == 0 && x25519(s2, b.p, pa) == 0 && memcmp(s1, s2, 32) == 0 &&
          eq(s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"), "and what they share", 0);
    free(a.p); free(b.p);

    for (unsigned i = 0; i < N(x25519_cases); i++) {
        const crypto_case_t* c = &x25519_cases[i];
        bytes_t pub = hex(c->f[0]), priv = hex(c->f[1]), want = hex(c->f[2]);
        int zero = 1;
        for (int j = 0; j < 32; j++) zero &= want.p[j] == 0;
        int r = x25519(out, priv.p, pub.p);
        check(memcmp(out, want.p, 32) == 0 && r == (zero ? -1 : 0), "X25519 (Wycheproof)", i);
        free(pub.p); free(priv.p); free(want.p);
    }
}

static void test_ct(void) {
    uint8_t a[40], b[40];
    for (int i = 0; i < 40; i++) a[i] = b[i] = (uint8_t)(i * 7);
    int ok = ct_equal(a, b, 40) == 1 && ct_equal(a, b, 0) == 1;
    for (int i = 0; i < 40; i++)
        for (int bit = 0; bit < 8; bit++) {
            b[i] ^= (uint8_t)(1 << bit);
            ok &= ct_equal(a, b, 40) == 0;
            b[i] ^= (uint8_t)(1 << bit);
        }
    check(ok, "ct_equal", 0);
    ct_wipe(a, sizeof a);
    int zero = 1;
    for (int i = 0; i < 40; i++) zero &= a[i] == 0;
    check(zero, "ct_wipe", 0);
}

int main(void) {
    test_sha256();
    test_hmac_hkdf();
    test_chacha20();
    test_poly1305();
    test_aead();
    test_x25519();
    test_ct();
    printf("test_crypto: %d of %d checks pass\n", checks - failures, checks);
    return failures ? 1 : 0;
}

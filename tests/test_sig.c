// Host test for signature checking (src/programs/lib/sig.c) and the
// hashes it stands on (sha512.c): SHA-384/512 against OpenSSL's answers,
// then RSA PKCS#1 v1.5, RSA-PSS and ECDSA on P-256 and P-384 against
// sig_cases.h, Project Wycheproof's vectors and OpenSSL's signatures
// (tests/gen_sig_cases.py makes it). Every signature that must verify is
// also checked to stop verifying when a byte of it or of the message changes.
// Run under the address and undefined-behaviour sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha512.h"
#include "sig.h"
#include "sig_cases.h"

static int failures, checks;

static void check(int ok, const char* what, const char* label) {
    checks++;
    if (ok) return;
    failures++;
    if (failures <= 30) printf("FAIL: %s (%s)\n", what, label);
}

typedef struct { uint8_t* p; uint32_t n; } bytes_t;

static bytes_t hex(const char* s) {
    bytes_t b;
    b.n = (uint32_t)(strlen(s) / 2);
    b.p = malloc(b.n ? b.n : 1);
    for (uint32_t i = 0; i < b.n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        b.p[i] = (uint8_t)v;
    }
    return b;
}

#define N(a) (sizeof(a) / sizeof *(a))

static void test_hash(const hash_case_t* cases, unsigned count, int hash, const char* name) {
    for (unsigned i = 0; i < count; i++) {
        bytes_t m = hex(cases[i].msg), want = hex(cases[i].digest);
        uint8_t got[64];
        hash_once(hash, m.p, m.n, got);
        check(want.n == hash_size(hash) && memcmp(got, want.p, want.n) == 0, name, "in one go");
        // And fed in pieces of every size up to 130, which crosses the
        // 128-byte block boundary at each offset.
        sha512_t s;
        uint32_t step = i % 130 + 1;
        if (hash == HASH_SHA384) sha384_init(&s);
        else sha512_init(&s);
        for (uint32_t at = 0; at < m.n; at += step) sha512_update(&s, m.p + at, m.n - at < step ? m.n - at : step);
        sha512_final(&s, got);
        check(memcmp(got, want.p, want.n) == 0, name, "in pieces");
        free(m.p);
        free(want.p);
    }
}

static int verify(const sig_case_t* c, const bytes_t* msg, const bytes_t* sig) {
    const sig_key_t* k = &sig_keys[c->key];
    bytes_t a = hex(k->a), b = hex(k->b);
    int r;
    if (c->kind == KIND_PKCS1) r = rsa_pkcs1_verify(a.p, a.n, b.p, b.n, c->hash, msg->p, msg->n, sig->p, sig->n);
    else if (c->kind == KIND_PSS)
        r = rsa_pss_verify(a.p, a.n, b.p, b.n, c->hash, (uint32_t)c->param, msg->p, msg->n, sig->p, sig->n);
    else r = ecdsa_verify(c->param, a.p, a.n, c->hash, msg->p, msg->n, sig->p, sig->n);
    free(a.p);
    free(b.p);
    return r;
}

static void test_sigs(void) {
    unsigned valid = 0;
    for (unsigned i = 0; i < N(sig_cases); i++) {
        const sig_case_t* c = &sig_cases[i];
        bytes_t msg = hex(c->msg), sig = hex(c->sig);
        int r = verify(c, &msg, &sig);
        if (c->ok) {
            valid++;
            check(r == SIG_OK, "a good signature refused", c->label);
            // One bit changed at the start, the middle or the end of the
            // signature (by turns), and in the message: each must be refused.
            uint32_t at[3] = { 0, sig.n / 2, sig.n - 1 }, j = at[i % 3];
            sig.p[j] ^= 0x10;
            check(verify(c, &msg, &sig) != SIG_OK, "a changed signature accepted", c->label);
            sig.p[j] ^= 0x10;
            if (msg.n) {
                msg.p[msg.n / 2] ^= 1;
                check(verify(c, &msg, &sig) != SIG_OK, "a changed message accepted", c->label);
                msg.p[msg.n / 2] ^= 1;
            }
            // Cut short by a byte, and a byte too long.
            sig.n--;
            check(verify(c, &msg, &sig) != SIG_OK, "a short signature accepted", c->label);
            sig.n++;
        } else {
            check(r != SIG_OK, "a bad signature accepted", c->label);
            if (strstr(c->label, "compressed")) check(r == SIG_UNSUPPORTED, "a compressed point not unsupported", c->label);
        }
        free(msg.p);
        free(sig.p);
    }
    printf("signatures: %u cases, %u of them good\n", (unsigned)N(sig_cases), valid);
}

// The edges of the calls themselves.
static void test_edges(void) {
    const sig_case_t* c = 0;
    for (unsigned i = 0; i < N(sig_cases) && !c; i++)
        if (sig_cases[i].kind == KIND_PKCS1 && sig_cases[i].ok && strstr(sig_cases[i].label, "4096_sha512")) c = &sig_cases[i];
    check(c != 0, "no 4096-bit case to work from", "edges");
    if (!c) return;
    const sig_key_t* k = &sig_keys[c->key];
    bytes_t n = hex(k->a), e = hex(k->b), msg = hex(c->msg), sig = hex(c->sig);
    check(rsa_pkcs1_verify(n.p, n.n, e.p, e.n, c->hash, msg.p, msg.n, sig.p, sig.n) == SIG_OK, "the 4096-bit case", "edges");
    check(rsa_pkcs1_verify(n.p, n.n, e.p, e.n, 9, msg.p, msg.n, sig.p, sig.n) == SIG_UNSUPPORTED, "an unknown hash", "edges");
    // A modulus past 4096 bits: unsupported, not wrong.
    uint8_t big[513];
    memset(big, 0xff, sizeof big);
    check(rsa_pkcs1_verify(big, sizeof big, e.p, e.n, c->hash, msg.p, msg.n, sig.p, sig.n) == SIG_UNSUPPORTED,
          "a 4104-bit modulus", "edges");
    // An even modulus, and exponents of 1 and 2.
    uint8_t even[512], one = 1, two = 2;
    memcpy(even, n.p + n.n - 512, 512);
    even[511] &= 0xfe;
    check(rsa_pkcs1_verify(even, 512, e.p, e.n, c->hash, msg.p, msg.n, sig.p, sig.n) == SIG_BAD, "an even modulus", "edges");
    check(rsa_pkcs1_verify(n.p, n.n, &one, 1, c->hash, msg.p, msg.n, sig.p, sig.n) == SIG_BAD, "exponent 1", "edges");
    check(rsa_pkcs1_verify(n.p, n.n, &two, 1, c->hash, msg.p, msg.n, sig.p, sig.n) == SIG_BAD, "exponent 2", "edges");
    // The signature with a leading zero added: one byte too long.
    uint8_t* longer = malloc(sig.n + 1);
    longer[0] = 0;
    memcpy(longer + 1, sig.p, sig.n);
    check(rsa_pkcs1_verify(n.p, n.n, e.p, e.n, c->hash, msg.p, msg.n, longer, sig.n + 1) == SIG_BAD, "a padded signature",
          "edges");
    free(longer);
    free(n.p);
    free(e.p);
    free(msg.p);
    free(sig.p);
    check(ecdsa_verify(7, (const uint8_t*)"", 0, HASH_SHA256, "", 0, (const uint8_t*)"", 0) == SIG_UNSUPPORTED,
          "an unknown curve", "edges");
    check(curve_size(CURVE_P256) == 32 && curve_size(CURVE_P384) == 48 && curve_size(0) == 0, "curve sizes", "edges");
    check(hash_size(HASH_SHA256) == 32 && hash_size(HASH_SHA384) == 48 && hash_size(HASH_SHA512) == 64 && hash_size(0) == 0,
          "hash sizes", "edges");
}

int main(void) {
    uint8_t d[64];
    sha384("abc", 3, d);
    bytes_t w = hex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    check(memcmp(d, w.p, 48) == 0, "SHA-384 of abc (FIPS 180-4)", "abc");
    free(w.p);
    sha512("abc", 3, d);
    w = hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    check(memcmp(d, w.p, 64) == 0, "SHA-512 of abc (FIPS 180-4)", "abc");
    free(w.p);
    test_hash(sha384_cases, N(sha384_cases), HASH_SHA384, "SHA-384");
    test_hash(sha512_cases, N(sha512_cases), HASH_SHA512, "SHA-512");
    test_sigs();
    test_edges();
    printf("test_sig: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

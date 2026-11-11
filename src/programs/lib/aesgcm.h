// AES-128-GCM (FIPS 197, NIST SP 800-38D): the AEAD every TLS 1.3 server
// must offer (TLS_AES_128_GCM_SHA256), for the servers that offer
// nothing else. Same shape as chachapoly.h, but the key is expanded once
// into an aesgcm_t, since AES's key schedule is not free.
//
// Nothing here looks anything up by a secret, and no branch depends on
// one: AES is bitsliced (the S-box is a circuit of ANDs and XORs, not a
// table, so the cache cannot tell which entries were used), and GHASH
// multiplies a bit at a time with masks.
#ifndef RINGTAIL_AESGCM_H
#define RINGTAIL_AESGCM_H

#include <stdint.h>
#include <stddef.h>

#define AESGCM_KEY_LEN 16
#define AESGCM_NONCE_LEN 12
#define AESGCM_TAG_LEN 16

typedef struct {
    uint32_t rk[11][8];  // the round keys, bitsliced for two blocks at once
    uint32_t h[4];       // GHASH's key: the zero block encrypted, as four big-endian words
} aesgcm_t;

// Expands a key. Wipe the result (ct_wipe) when done with it.
void aesgcm_init(aesgcm_t* k, const uint8_t key[AESGCM_KEY_LEN]);

// AES-128 on two blocks at once (in and out may be the same). For the tests.
void aes128_encrypt2(const aesgcm_t* k, const uint8_t in[32], uint8_t out[32]);

// Encrypts len bytes of plain into cipher (which may be the same buffer)
// and writes the tag, which covers the cipher text and aad. A nonce must
// never be used twice with one key.
void aesgcm_seal(const aesgcm_t* k, const uint8_t nonce[AESGCM_NONCE_LEN], const void* aad, size_t aadlen,
                 const uint8_t* plain, size_t len, uint8_t* cipher, uint8_t tag[AESGCM_TAG_LEN]);

// The reverse. 0 with the plain text written (plain may be cipher), or -1
// when the tag does not match, and then nothing is written.
int aesgcm_open(const aesgcm_t* k, const uint8_t nonce[AESGCM_NONCE_LEN], const void* aad, size_t aadlen,
                const uint8_t* cipher, size_t len, const uint8_t tag[AESGCM_TAG_LEN], uint8_t* plain);

#endif

// ChaCha20-Poly1305 (RFC 8439): encryption that also proves the data,
// and some extra data sent in the clear beside it, were not changed on
// the way. TLS 1.3 protects every record with it.
#ifndef RINGTAIL_CHACHAPOLY_H
#define RINGTAIL_CHACHAPOLY_H

#include <stdint.h>
#include <stddef.h>

#define CHACHAPOLY_KEY_LEN 32
#define CHACHAPOLY_NONCE_LEN 12
#define CHACHAPOLY_TAG_LEN 16

// Poly1305 alone: a 16-byte checksum of a message under a 32-byte key
// that must never be used for a second message.
typedef struct {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    uint32_t have;
} poly1305_t;

void poly1305_init(poly1305_t* p, const uint8_t key[32]);
void poly1305_update(poly1305_t* p, const void* data, size_t len);
void poly1305_final(poly1305_t* p, uint8_t tag[16]);

// Encrypts len bytes of plain into cipher (which may be the same
// buffer) and writes the tag, which covers the cipher text and aad. A
// nonce must never be used twice with one key.
void chachapoly_seal(const uint8_t key[CHACHAPOLY_KEY_LEN], const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                     const void* aad, size_t aadlen, const uint8_t* plain, size_t len,
                     uint8_t* cipher, uint8_t tag[CHACHAPOLY_TAG_LEN]);

// The reverse. 0 with the plain text written (plain may be cipher), or
// -1 when the tag does not match: then something was changed, or the
// key is wrong, and nothing is written.
int chachapoly_open(const uint8_t key[CHACHAPOLY_KEY_LEN], const uint8_t nonce[CHACHAPOLY_NONCE_LEN],
                    const void* aad, size_t aadlen, const uint8_t* cipher, size_t len,
                    const uint8_t tag[CHACHAPOLY_TAG_LEN], uint8_t* plain);

#endif

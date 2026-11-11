// SHA-256 (FIPS 180-4), and the two things built on it that TLS needs:
// HMAC (RFC 2104), a keyed checksum, and HKDF (RFC 5869), which turns
// one secret into as many keys as are wanted. Shared by the kernel
// (the random number generator) and user programs.
#ifndef RINGTAIL_SHA256_H
#define RINGTAIL_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t len;     // bytes hashed so far
    uint8_t buf[64];  // a block not yet full
} sha256_t;

void sha256_init(sha256_t* s);
void sha256_update(sha256_t* s, const void* data, size_t len);
// The hash of everything so far. The context is wiped; to go on hashing
// after reading a hash (TLS does), finish a copy.
void sha256_final(sha256_t* s, uint8_t out[SHA256_LEN]);
void sha256(const void* data, size_t len, uint8_t out[SHA256_LEN]);

typedef struct {
    sha256_t inner, outer;
} hmac_sha256_t;

void hmac_sha256_init(hmac_sha256_t* h, const void* key, size_t keylen);
void hmac_sha256_update(hmac_sha256_t* h, const void* data, size_t len);
void hmac_sha256_final(hmac_sha256_t* h, uint8_t out[SHA256_LEN]);
void hmac_sha256(const void* key, size_t keylen, const void* data, size_t len, uint8_t out[SHA256_LEN]);

// HKDF's two steps. Extract boils input keying material down to one
// 32-byte key (a NULL salt counts as 32 zero bytes). Expand stretches
// such a key into outlen bytes that depend on `info`; outlen is at most
// 255 * 32, and more than that gives -1 and writes nothing.
void hkdf_sha256_extract(const void* salt, size_t saltlen, const void* ikm, size_t ikmlen, uint8_t prk[SHA256_LEN]);
int hkdf_sha256_expand(const uint8_t prk[SHA256_LEN], const void* info, size_t infolen, uint8_t* out, size_t outlen);

#endif

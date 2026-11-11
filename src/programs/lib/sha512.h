// SHA-512 and SHA-384 (FIPS 180-4): the longer hashes that certificates
// and servers sign with (TLS 1.3 itself uses SHA-256 here, see
// sha256.h). SHA-384 is SHA-512 started from other values and cut short.
#ifndef RINGTAIL_SHA512_H
#define RINGTAIL_SHA512_H

#include <stdint.h>
#include <stddef.h>

#define SHA512_LEN 64
#define SHA384_LEN 48

typedef struct {
    uint64_t h[8];
    uint64_t len;      // bytes hashed so far (2^64 is plenty)
    uint8_t buf[128];  // a block not yet full
    uint32_t out_len;  // 64 or 48
} sha512_t;

void sha512_init(sha512_t* s);
void sha384_init(sha512_t* s);
void sha512_update(sha512_t* s, const void* data, size_t len);
// Writes out_len bytes (64, or 48 for SHA-384) and wipes the context.
void sha512_final(sha512_t* s, uint8_t* out);
void sha512(const void* data, size_t len, uint8_t out[SHA512_LEN]);
void sha384(const void* data, size_t len, uint8_t out[SHA384_LEN]);

#endif

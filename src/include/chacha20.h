// The ChaCha20 stream cipher (RFC 8439): a key, a nonce and a block
// counter make a stream of bytes that is XORed onto the data. Shared by
// the kernel (the random number generator) and user programs (TLS).
#ifndef RINGTAIL_CHACHA20_H
#define RINGTAIL_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

#define CHACHA20_KEY_LEN 32
#define CHACHA20_NONCE_LEN 12

// XORs the key stream, starting at block `counter`, onto len bytes of
// in, giving out (which may be in itself). A NULL in gives the key
// stream alone. The same call encrypts and decrypts.
void chacha20_xor(const uint8_t key[CHACHA20_KEY_LEN], uint32_t counter, const uint8_t nonce[CHACHA20_NONCE_LEN],
                  const uint8_t* in, uint8_t* out, size_t len);

#endif

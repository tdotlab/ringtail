// X25519 (RFC 7748): the key agreement TLS 1.3 starts with. Each side
// picks 32 secret bytes and sends the matching public value; each then
// combines its own secret with the other's public value and both arrive
// at the same 32 bytes, which someone who saw only the public values
// cannot work out.
#ifndef RINGTAIL_X25519_H
#define RINGTAIL_X25519_H

#include <stdint.h>

#define X25519_LEN 32

// The public value for a secret (any 32 random bytes).
void x25519_public(uint8_t pub[X25519_LEN], const uint8_t secret[X25519_LEN]);

// The shared value from our secret and their public value. 0, or -1
// when the result is all zeros: the other side sent one of the few
// values that give the same answer whatever our secret is, and the
// result must not be used.
int x25519(uint8_t shared[X25519_LEN], const uint8_t secret[X25519_LEN], const uint8_t their_pub[X25519_LEN]);

#endif

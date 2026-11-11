// Checking signatures: RSA (PKCS#1 v1.5 and PSS, RFC 8017) and ECDSA on
// the curves P-256 and P-384 (FIPS 186-5, SEC 1), with SHA-256, -384 and
// -512. What a certificate chain and a TLS server sign with.
//
// Only checking: nothing here signs, and everything it handles is public
// (keys, messages, signatures), so the arithmetic need not take the same
// time whatever the numbers; it is written to be plainly right instead.
#ifndef RINGTAIL_SIG_H
#define RINGTAIL_SIG_H

#include <stdint.h>
#include <stddef.h>

enum { HASH_SHA256 = 1, HASH_SHA384, HASH_SHA512 };
#define HASH_MAX_LEN 64

// The size of a hash's output (0 for one that is not known).
uint32_t hash_size(int hash);
void hash_once(int hash, const void* data, size_t len, uint8_t* out);

#define SIG_OK          0
#define SIG_BAD         (-1)  // the signature does not verify, or is malformed
#define SIG_UNSUPPORTED (-2)  // a key or parameter outside what is handled here

// RSA: the modulus and public exponent as big-endian bytes (leading
// zeros allowed), moduli of up to 4096 bits. The message is hashed here.
int rsa_pkcs1_verify(const uint8_t* n, uint32_t n_len, const uint8_t* e, uint32_t e_len, int hash, const void* msg,
                     size_t msg_len, const uint8_t* sig, uint32_t sig_len);
// PSS, with MGF1 on the same hash and a salt of salt_len bytes (TLS:
// the hash's length).
int rsa_pss_verify(const uint8_t* n, uint32_t n_len, const uint8_t* e, uint32_t e_len, int hash, uint32_t salt_len,
                   const void* msg, size_t msg_len, const uint8_t* sig, uint32_t sig_len);

enum { CURVE_P256 = 1, CURVE_P384 };

// The size of a curve's numbers in bytes (0 for one that is not known).
uint32_t curve_size(int curve);

// ECDSA: the public key as a point, uncompressed (04 || x || y); the
// signature as DER, SEQUENCE { r INTEGER, s INTEGER }. The key is
// checked to be a point on the curve.
int ecdsa_verify(int curve, const uint8_t* point, uint32_t point_len, int hash, const void* msg, size_t msg_len,
                 const uint8_t* sig, uint32_t sig_len);

#endif

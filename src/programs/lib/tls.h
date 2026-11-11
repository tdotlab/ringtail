// A TLS 1.3 client (RFC 8446): an encrypted connection over any stream
// of bytes. Two cipher suites, TLS_CHACHA20_POLY1305_SHA256 (offered
// first) and TLS_AES_128_GCM_SHA256 (which every server must take), and
// one key exchange, X25519; servers that only speak TLS 1.2, or take
// neither, are turned down with a message that says so.
//
// Who the server is, it leaves to a verifier the caller gives
// (tls_set_verify): handed the certificate chain and the server's
// signature over the handshake, it says yes or no, and on a no the
// handshake ends with the alert that fits. Without one, the connection
// is encrypted and nobody listening can read it, but nothing proves who
// is at the other end: tls_peer_verified() says 0 so that a program can
// tell its user.
//
// No system calls in here: bytes come and go through three functions
// the caller gives, so the host tests run this same code against
// recorded and scripted servers.
#ifndef RINGTAIL_TLS_H
#define RINGTAIL_TLS_H

#include <stdint.h>

typedef struct {
    // Up to n bytes into buf: how many (at least 1), 0 when the other
    // side has closed, or a negative error, which is passed on.
    int (*read)(void* ctx, void* buf, uint32_t n);
    // All n bytes: 0, or a negative error, which is passed on.
    int (*write)(void* ctx, const void* buf, uint32_t n);
    // n bytes nobody can predict (a key is made from them): 0, or a
    // negative error.
    int (*random)(void* ctx, void* buf, uint32_t n);
    void* ctx;
} tls_io_t;

typedef struct tls tls_t;

// Errors are negative. Those the callbacks return pass through as they
// are; the rest are these, kept clear of errno values.
#define TLS_E_EOF       (-1001)  // the connection closed in the middle of things
#define TLS_E_NOT_TLS   (-1002)  // what came back is not TLS at all
#define TLS_E_VERSION   (-1003)  // the server does not speak TLS 1.3
#define TLS_E_PROTOCOL  (-1004)  // a message that is not allowed at that point
#define TLS_E_DECODE    (-1005)  // a message that does not parse
#define TLS_E_PARAM     (-1006)  // the server chose something that was not on offer
#define TLS_E_BAD_MAC   (-1007)  // a record failed its integrity check: damaged or forged
#define TLS_E_FINISHED  (-1008)  // the handshake's checksum is wrong: someone meddled with it
#define TLS_E_TOO_BIG   (-1009)  // a record or message over the size limit
#define TLS_E_NOMEM     (-1010)
#define TLS_E_CLOSED    (-1011)  // used after it was closed or had failed
#define TLS_E_NAME      (-1012)  // the server name is too long or malformed
// What a verifier returns to turn the server down, and the alert each sends.
#define TLS_E_CERT_BAD         (-1020)  // a certificate that is malformed or fails a check (bad_certificate)
#define TLS_E_CERT_UNSUPPORTED (-1021)  // a kind of key or certificate we cannot check (unsupported_certificate)
#define TLS_E_CERT_EXPIRED     (-1022)  // out of its dates (certificate_expired)
#define TLS_E_CERT_UNKNOWN_CA  (-1023)  // no chain to a trusted root (unknown_ca)
#define TLS_E_CERT_NAME        (-1024)  // not a certificate for the name asked for (bad_certificate)
#define TLS_E_CERT_SIGNATURE   (-1025)  // the signature over the handshake is wrong (decrypt_error)
// The server sent a fatal alert: TLS_E_ALERT minus its number (RFC 8446
// section 6), so -2040 is handshake_failure.
#define TLS_E_ALERT     (-2000)

// A new connection over io (copied). NULL when memory runs out.
tls_t* tls_new(const tls_io_t* io);
// Wipes the keys and frees everything. Does not say goodbye: tls_close.
void tls_free(tls_t* t);

// What a verifier is handed, all of it valid only during the call.
typedef struct {
    const char* name;              // the name connected to, as given (an IPv4 address too); "" for none
    uint32_t n_certs;              // at least 1
    const uint8_t* const* cert;    // each certificate, DER: the server's own first, then
    const uint32_t* cert_len;      //   whatever else it sent, in its order, unchecked
    uint16_t scheme;               // how the server signed (RFC 8446 4.2.3): one we offered, never PKCS#1
    const uint8_t* signed_content; // what it signed (4.4.3), built here from the handshake
    uint32_t signed_len;
    const uint8_t* signature;      // and its signature, as sent
    uint32_t signature_len;
} tls_peer_t;

// 0 to accept the server: its chain is good for `name` and the
// signature verifies under the first certificate's key. Otherwise one of
// the TLS_E_CERT_ errors (any other negative value is passed on, with an
// internal_error alert; anything else nonzero counts as TLS_E_CERT_BAD).
typedef int (*tls_verify_fn)(void* ctx, const tls_peer_t* peer);

// Set before tls_connect. It is called once, before the server's Finished.
void tls_set_verify(tls_t* t, tls_verify_fn fn, void* ctx);

// The handshake. server_name is sent to the server so it can pick the
// right site (it is not sent for an IPv4 address, or when NULL). 0, or
// an error; after an error the connection is dead.
int tls_connect(tls_t* t, const char* server_name);

// Up to n bytes of what the server sent: how many (at least 1), 0 when
// the server has said it is done (so nothing was cut off), or an error;
// TLS_E_EOF when the connection just stopped, which may mean someone
// cut it short.
int tls_read(tls_t* t, void* buf, uint32_t n);
// Sends all n bytes: n, or an error.
int tls_write(tls_t* t, const void* buf, uint32_t n);
// Tells the server we are done sending. 0, or an error.
int tls_close(tls_t* t);

// Whether the server proved who it is: 1 only when a verifier was set
// and accepted it.
int tls_peer_verified(const tls_t* t);

// Text for any error the calls above return, errno values included.
const char* tls_strerror(int err);

// HKDF-Expand-Label (RFC 8446 section 7.1), which every key in TLS 1.3
// comes out of. Here so the tests can check it against the RFC's
// worked example.
void tls13_expand_label(const uint8_t secret[32], const char* label, const uint8_t* context, uint32_t context_len,
                        uint8_t* out, uint32_t out_len);

#endif

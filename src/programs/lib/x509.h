// Certificates (X.509, RFC 5280, as the Web PKI profiles it): reading
// them, finding a path from the server's certificate to a trusted root,
// and checking that path and the server's name. And a verifier for
// tls_set_verify() built from those, which also checks the server's
// CertificateVerify signature with the key its certificate holds.
//
// What is checked: every signature on the path; dates, against the
// caller's clock, for every certificate, the root's included; the CA flag
// (basicConstraints, critical), path lengths, key usage and extended key
// usage (serverAuth); name constraints on DNS names, IP addresses and
// directory names; the name, against subjectAltName only (DNS names with a
// whole leftmost-label wildcard, and IP addresses), never the common
// name. Refused: unknown critical extensions, version 1 certificates,
// SHA-1, RSA keys under 2048 bits, curves other than P-256 and P-384.
// Not done: revocation, certificate transparency, certificate policies.
//
// Errors are the TLS_E_CERT_* codes of tls.h, so a verifier's answer
// carries straight through to the right alert.
#ifndef RINGTAIL_X509_H
#define RINGTAIL_X509_H

#include <stdint.h>
#include "tls.h"

typedef struct {
    const uint8_t* p;
    uint32_t n;
} x509_span_t;

enum { X509_KEY_NONE, X509_KEY_RSA, X509_KEY_EC };

// Key usage bits (RFC 5280 4.2.1.3).
#define X509_KU_DIGITAL_SIGNATURE (1u << 0)
#define X509_KU_KEY_CERT_SIGN     (1u << 5)

// A certificate as read: spans point into the bytes it was read from,
// which must outlive it.
typedef struct {
    x509_span_t der, tbs, sig;
    int sig_kind, sig_hash;          // how the issuer signed it: 0 if not a way handled here
    x509_span_t serial;              // the INTEGER's contents
    x509_span_t issuer, subject;     // each a whole Name, tag and length included
    uint64_t not_before, not_after;  // see x509_time()
    int key_type, curve;             // X509_KEY_*; CURVE_* for EC keys
    x509_span_t key_a, key_b;        // RSA: n, e. EC: the point
    uint32_t key_bits;
    int has_bc, bc_critical, is_ca, path_len;  // path_len -1: none
    int has_ku;
    uint32_t ku;
    int has_eku, eku_server_auth, eku_any;
    int has_san, san_critical;
    x509_span_t san;  // the GeneralNames' contents
    int has_nc;
    x509_span_t nc_permitted, nc_excluded;  // GeneralSubtrees' contents (n 0: absent)
} x509_t;

// A time as the number YYYYMMDDhhmmss, so times compare as numbers.
uint64_t x509_time(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t min, uint32_t sec);

// Reads one DER certificate (exactly len bytes). 0, TLS_E_CERT_BAD for
// anything malformed, or TLS_E_CERT_UNSUPPORTED (a version other than 3,
// an unknown critical extension).
int x509_parse(x509_t* c, const uint8_t* der, uint32_t len);

// Whether issuer's key signed child (and is a key strong enough to trust): 0 or a TLS_E_CERT_*.
int x509_check_signature(const x509_t* issuer, const x509_t* child);

// Trusted roots, kept in memory.
typedef struct {
    x509_t* cert;
    uint8_t** der;
    uint32_t n, cap;
} x509_roots_t;

// Adds one root from DER (copied). 0, or the reason it was not added.
int x509_roots_add(x509_roots_t* r, const uint8_t* der, uint32_t len);
// Adds every CERTIFICATE in PEM text; returns how many were added (any
// that do not read are skipped), or -1 when out of memory.
int x509_roots_add_pem(x509_roots_t* r, const char* pem, uint32_t len);
void x509_roots_free(x509_roots_t* r);

#define X509_MAX_DEPTH 6  // what browsers meet in practice is 1 to 3

typedef struct {
    const x509_roots_t* roots;
    uint64_t now;          // x509_time() of the moment to check against
    uint32_t max_depth;    // intermediates allowed, self-issued ones not counted (X509_MAX_DEPTH)
    uint32_t leaf_ku;      // key usage bits the leaf must have, if it lists any
} x509_policy_t;

// Checks the certificates a server sent (cert[0] its own, the rest in any
// order, any of them unneeded) up to a root, and that the leaf is for this
// name: a DNS name (dns), or an IP address (ip, 4 or 16 bytes; dns 0).
int x509_verify_chain(const x509_policy_t* pol, const uint8_t* const* cert, const uint32_t* cert_len, uint32_t n,
                      const char* dns, const uint8_t* ip, uint32_t ip_len);

// A tls_verify_fn: ctx is an x509_policy_t. Checks the chain against the
// name the client asked for, then the CertificateVerify signature with the
// leaf's key (the scheme must fit the key: P-256 for 0x0403, P-384 for
// 0x0503, RSA for RSA-PSS).
int x509_tls_verify(void* ctx, const tls_peer_t* peer);

#endif

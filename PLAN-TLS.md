# TLS: where it stands, and what is left

## Done

**v0.25.0 "Trust Me"**: a TLS 1.3 client (`src/programs/lib/tls.c`):
ChaCha20-Poly1305, X25519, SHA-256/HMAC/HKDF, kernel random numbers,
`fetch https://` over HTTP/1.1. Host-tested against 185 recorded
conversations (a scripted server and OpenSSL), Wycheproof vectors for the
primitives, and QEMU tests against a Python TLS server.

**The verifier hook** (commit after v0.25.0): `tls_set_verify()` hands a
caller's verifier a `tls_peer_t` (the name asked for, every certificate
the server sent, the CertificateVerify scheme and signature, and the
signed content, built inside `tls.c` from the transcript). Its answer
decides the handshake; each `TLS_E_CERT_*` sends the RFC's alert, and
anything unclear fails closed. `tls_peer_verified()` is 1 only after a
yes. Offered schemes: ECDSA P-256/SHA-256 and P-384/SHA-384, RSA-PSS with
SHA-256/384/512, and PKCS#1 v1.5 for certificates only. 210 recorded
conversations, each checked by the generator against Python's
`cryptography`: the client's signed content verifies under the server's
key, for OpenSSL with P-256, P-384 and RSA keys.

## Left to do (in order)

1. **SHA-384/512** (`src/programs/lib/sha512.c`), vectors from OpenSSL.
2. **Big numbers**: Montgomery multiplication over 32-bit limbs, for any
   odd modulus up to 4096 bits. Only public data passes through it, so
   it need not run in constant time.
3. **RSA** verify: PKCS#1 v1.5 by encoding the expected block and
   comparing; PSS with MGF1 on the same hash and a salt as long as the
   hash. **ECDSA** verify on P-256 and P-384 (Jacobian coordinates, the
   doubling case in u1*G + u2*Q handled, r and s in [1, n-1], point on
   the curve). All against Wycheproof (`rsa_signature_*`, `rsa_pss_*`,
   `ecdsa_secp256r1_*`, `ecdsa_secp384r1_*`), keys taken through our own
   SubjectPublicKeyInfo parser.
4. **DER and X.509**: a strict DER reader; certificates with the
   extensions that matter (basicConstraints, keyUsage, extKeyUsage,
   subjectAltName, nameConstraints); unknown critical extensions refused.
5. **Path validation**: build from the leaf through any of the sent
   certificates to a root (byte-equal names, signature checked at each
   step, depth limit), dates against the clock, CA flag, path length, key
   usage, EKU serverAuth, name constraints (DNS, IP), SHA-1 refused, RSA
   at least 2048 bits. **Names**: subjectAltName only (DNS with a
   leftmost-label wildcard, IP addresses), never the common name.
   Tested against C2SP x509-limbo (the cases within this scope) with
   Python's `cryptography` verifier as a second opinion.
6. **Roots**: Mozilla's list (as the system's ca-certificates packages
   it) vendored as PEM and copied onto the disk; `fetch` loads it.
7. **fetch** verifies by default; `-k` skips it, with the existing
   warning; `--ca FILE` trusts a file in place of the roots (the QEMU
   tests need it, and so does this sandbox, whose egress gateway is a
   TLS man-in-the-middle with its own CA).
8. Docs (README, PROGRESS), version, full `make test`, release.

Not planned: revocation (CRL, OCSP), certificate transparency, policies.

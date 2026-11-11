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

**v0.26.0 "Prove It"**: the certificate is checked, by default.
`sha512.c` (SHA-384/512); `sig.c`, a Montgomery big-number library
under RSA PKCS#1 v1.5, RSA-PSS and ECDSA on P-256 and P-384, tested
against 3303 Wycheproof and OpenSSL cases; `x509.c`, a strict DER
reader, certificates, path building through any of the sent
certificates to a root, and every check on the path (signatures, dates,
CA flag, path length, key usage, EKU, name constraints on DNS, IP and
directory names, the name against subjectAltName only), tested against
x509-limbo and 228 crafted cases each with its expected error, then
mutation-swept. Mozilla's roots are `/roots.pem` on the disk
(`tools/mkroots.py`, taken from the Mozilla directory only, since the
system bundle here also holds the sandbox's own proxy CAs). `fetch`
verifies by default; `-k` skips it with the warning, `--ca FILE` trusts
a file in place of the roots. The QEMU tests make their own PKI and
check refusals (unknown CA, wrong name, expired) as well as success.

## Left

Not planned: revocation (CRL, OCSP), certificate transparency, policies.
Done since: AES-128-GCM (v0.27.0), for the servers that offer nothing
else. Possible later: session
resumption, ALPN, a way to update the roots without rebuilding the disk.

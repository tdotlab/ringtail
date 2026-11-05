# Plan: TLS from scratch

Not started. The goal is `fetch https://` against real servers, with
everything written for RingTail rather than ported.

## Scope

TLS 1.3 only, client only. One cipher suite, TLS_CHACHA20_POLY1305_SHA256,
and one key exchange, X25519; almost every major server accepts both.
No TLS 1.2, no AES, no server side, no session resumption (tickets are
read and ignored), no client certificates.

Why this choice: TLS 1.3 removed most of 1.2's dangerous options (RSA
key exchange, renegotiation, CBC). ChaCha20, Poly1305 and X25519 run in
constant time when written the obvious way, which removes the timing
leaks that usually sink homemade crypto. AES is the one where the
obvious version leaks, so it stays out. Checking certificates touches
only public data, so timing does not matter there.

## Pieces, easiest first

- **Hashes and key derivation**: SHA-256, SHA-384 (certificate
  signatures need it), HMAC, HKDF. Checked against the published test
  values.
- **ChaCha20-Poly1305 and X25519**: RFC 8439 and RFC 7748, with their
  test vectors. TweetNaCl shows how small these can be.
- **The protocol**: the record layer, then ClientHello (SNI,
  supported_versions, key_share, signature_algorithms), ServerHello,
  HelloRetryRequest, EncryptedExtensions, Certificate, CertificateVerify
  and Finished. Then the key schedule, alerts, close_notify and
  KeyUpdate, plus the compatibility quirks (a fake ChangeCipherSpec and
  legacy_session_id) that servers and middleboxes expect.
- **Certificates, the swamp**: DER/ASN.1 and X.509 parsing. Building
  the chain to a trusted root. Checking the name against subjectAltName
  (wildcards included), the validity dates against the RTC,
  basicConstraints and key usage. Verifying signatures with RSA
  (PKCS#1 v1.5 and PSS, up to 4096 bits) and ECDSA on P-256 and P-384,
  because Let's Encrypt chains use both.

## Kernel and system prerequisites

- **A real random-number generator**: a CSPRNG in the kernel, seeded
  from timer jitter, interrupt timing and RDRAND where the CPU has it,
  and offered through a `getrandom` syscall. Weak randomness breaks TLS
  silently. `net_random` is not good enough.
- **A CA bundle on the disk**, and code to load it.
- The clock already exists (the RTC), so expiry checks are covered.

TLS itself lives in user space (`src/programs/lib`), linked into
`fetch`. The kernel barely changes.

## Order of work

1. The primitives, with host tests in the style of `tests/test_tcp.c`.
2. The handshake, checked byte for byte against the example handshakes
   in RFC 8448, then against a local `openssl s_server`.
3. Certificate validation against a test CA of our own, then against a
   pile of bad certificates: expired, wrong name, bad signature,
   untrusted root, an intermediate that may not sign, and a broken
   chain. Project Wycheproof's vectors for RSA and ECDSA.
4. The random-number generator, the CA bundle on disk, and `fetch
   https://`. The QEMU tests run against a Python TLS server on the
   host using the test CA.

## Risks

TCP bugs show up loudly, as hangs and stalls. TLS bugs here are mostly
silent: a certificate check that accepts a forgery still fetches the
page. So most of the tests must be negative ones. Skipping certificate
checks gives encryption without knowing who is on the other end. That
is allowed only behind a flag that is never the default, and only
during step 2.

Testing from the cloud sandbox: its outbound proxy is a TLS
man-in-the-middle, so tests against real internet servers are not
meaningful there. Test locally.

## Size

About two to four sessions; a BearSSL port would be about one. The
certificate phase is the biggest and needs the most care.

Before this, optionally: a small `httpd`, to put the TCP stack to real
use and shake out bugs before TLS depends on it.

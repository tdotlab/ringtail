// A TLS 1.3 client: see tls.h. Section numbers are RFC 8446's.
//
// Three layers, bottom up. Records (section 5): everything travels in
// records of at most 16 KB, and once keys exist each record is sealed
// with ChaCha20-Poly1305, its true type hidden inside. Handshake
// messages (section 4) ride in records, several to a record or one
// across many. The handshake itself: we send ClientHello with an X25519
// public value; the server answers with its own, and from there on
// everything is encrypted under keys both sides work out from the
// shared X25519 result and a hash of every handshake message so far.
// Each side ends with a Finished message, a keyed checksum over that
// same hash, so if anyone changed a byte of the handshake on the way
// the two hashes differ and the connection dies here.
//
// A server that breaks a rule gets the alert the RFC names for it and
// the connection is dead from then on: no error is ever worked around.
#include "tls.h"
#include "sha256.h"
#include "chachapoly.h"
#include "aesgcm.h"
#include "x25519.h"
#include "ct.h"
#include "lib.h"

void* malloc(size_t size);
void* realloc(void* p, size_t size);
void free(void* p);

#define HEADER 5                       // type, two version bytes, two length bytes
#define MAX_PLAIN 16384                // most a record may carry (section 5.1)
#define MAX_CIPHER (MAX_PLAIN + 256)   // and most a sealed one may be (5.2)
#define TAG CHACHAPOLY_TAG_LEN
#define MAX_HANDSHAKE (128 * 1024)     // longest handshake message taken: room for a long certificate chain
#define HASH SHA256_LEN

enum { REC_CCS = 20, REC_ALERT = 21, REC_HANDSHAKE = 22, REC_APPDATA = 23 };
enum {
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
    HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20,
    HS_KEY_UPDATE = 24,
};
enum {
    EXT_SERVER_NAME = 0, EXT_SUPPORTED_GROUPS = 10, EXT_SIGNATURE_ALGORITHMS = 13, EXT_SUPPORTED_VERSIONS = 43,
    EXT_COOKIE = 44, EXT_KEY_SHARE = 51,
};
enum {
    AL_CLOSE_NOTIFY = 0, AL_UNEXPECTED_MESSAGE = 10, AL_BAD_RECORD_MAC = 20, AL_RECORD_OVERFLOW = 22,
    AL_BAD_CERTIFICATE = 42, AL_UNSUPPORTED_CERTIFICATE = 43, AL_CERTIFICATE_EXPIRED = 45, AL_UNKNOWN_CA = 48,
    AL_ILLEGAL_PARAMETER = 47, AL_DECODE_ERROR = 50, AL_DECRYPT_ERROR = 51, AL_PROTOCOL_VERSION = 70,
    AL_INTERNAL_ERROR = 80, AL_USER_CANCELED = 90, AL_MISSING_EXTENSION = 109, AL_UNSUPPORTED_EXTENSION = 110,
};
#define NO_ALERT (-1)

#define TLS13 0x0304
#define SUITE_AES_128_GCM_SHA256 0x1301
#define SUITE_CHACHA20_POLY1305_SHA256 0x1303
#define GROUP_X25519 0x001d

// One direction's keys. The key and iv come from the secret (7.3); the
// secret is kept because a key update makes the next one from it (7.2).
// Both suites take the same hash, so only the AEAD differs: ChaCha20-
// Poly1305 uses key, AES-128-GCM its expansion, gcm.
typedef struct {
    uint8_t secret[HASH];
    uint32_t suite;
    uint8_t key[CHACHAPOLY_KEY_LEN], iv[CHACHAPOLY_NONCE_LEN];
    aesgcm_t gcm;
    uint64_t seq;  // records sealed or opened under this key
    int on;
} keys_t;

enum { ST_NEW, ST_OPEN, ST_EOF, ST_DEAD };  // ST_EOF: the server said goodbye; we may still send

struct tls {
    tls_io_t io;
    int state, err;
    int got_record;   // something that looked like TLS has arrived
    int hello_sent;   // the first record has gone out (it alone carries the old version number)
    int ccs_ok;       // the handshake is at a point where a stray ChangeCipherSpec is to be dropped (5)
    int sent_ccs, sent_close;
    keys_t rd, wr;
    uint32_t suite;   // the one the server chose
    sha256_t transcript;  // the hash of every handshake message so far
    uint8_t master[HASH];
    uint8_t x_secret[X25519_LEN], x_public[X25519_LEN];
    uint8_t random[32], session_id[32];
    char name[256];   // the name for the server (none for an address)
    char asked[256];  // the name or address the caller gave, for the verifier
    tls_verify_fn verify;
    void* verify_ctx;
    int verified;
    uint8_t* certs;   // a copy of the Certificate message's body, kept for the verifier
    uint32_t certs_len;
    uint8_t* hs;      // handshake bytes received and not yet used up: hs[hs_at .. hs_len)
    uint32_t hs_at, hs_len, hs_cap;
    uint32_t app_at, app_len;  // application data opened and not yet handed out, in `in`
    uint8_t in[HEADER + MAX_CIPHER];
    uint8_t out[HEADER + MAX_PLAIN + 1 + TAG];
};

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

// ---- The key schedule (section 7.1) ----

void tls13_expand_label(const uint8_t secret[32], const char* label, const uint8_t* context, uint32_t context_len,
                        uint8_t* out, uint32_t out_len) {
    // struct { uint16 length; opaque label<7..255> = "tls13 " + label; opaque context<0..255>; }
    uint8_t info[2 + 1 + 6 + 32 + 1 + HASH];
    uint32_t n = 0, ll = (uint32_t)strlen(label);
    put16(info, out_len);
    n = 2;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)context_len;
    if (context_len) memcpy(info + n, context, context_len);
    n += context_len;
    hkdf_sha256_expand(secret, info, n, out, out_len);
}

static void transcript_hash(const tls_t* t, uint8_t out[HASH]) {
    sha256_t copy = t->transcript;
    sha256_final(&copy, out);
}

// Derive-Secret: a new secret from an old one, a label and the handshake so far.
static void derive_secret(const tls_t* t, const uint8_t secret[HASH], const char* label, uint8_t out[HASH]) {
    uint8_t h[HASH];
    transcript_hash(t, h);
    tls13_expand_label(secret, label, h, HASH, out, HASH);
}

static void set_keys(keys_t* k, uint32_t suite, const uint8_t secret[HASH]) {
    memcpy(k->secret, secret, HASH);
    k->suite = suite;
    if (suite == SUITE_AES_128_GCM_SHA256) {
        tls13_expand_label(secret, "key", 0, 0, k->key, AESGCM_KEY_LEN);
        aesgcm_init(&k->gcm, k->key);
        ct_wipe(k->key, sizeof k->key);
    } else {
        tls13_expand_label(secret, "key", 0, 0, k->key, sizeof k->key);
    }
    tls13_expand_label(secret, "iv", 0, 0, k->iv, sizeof k->iv);
    k->seq = 0;
    k->on = 1;
}

// The verify_data of a Finished message (4.4.4): an HMAC of the
// handshake so far, under a key made from the sender's handshake secret.
static void finished_mac(const tls_t* t, const uint8_t secret[HASH], uint8_t out[HASH]) {
    uint8_t key[HASH], h[HASH];
    tls13_expand_label(secret, "finished", 0, 0, key, HASH);
    transcript_hash(t, h);
    hmac_sha256(key, HASH, h, HASH, out);
    ct_wipe(key, sizeof key);
}

// A record sealed or opened with the suite's AEAD.
static void seal(const keys_t* k, const uint8_t* nonce, const uint8_t* aad, uint32_t aadlen, uint8_t* data,
                 uint32_t len, uint8_t* tag) {
    if (k->suite == SUITE_AES_128_GCM_SHA256) aesgcm_seal(&k->gcm, nonce, aad, aadlen, data, len, data, tag);
    else chachapoly_seal(k->key, nonce, aad, aadlen, data, len, data, tag);
}

static int open_(const keys_t* k, const uint8_t* nonce, const uint8_t* aad, uint32_t aadlen, uint8_t* data,
                 uint32_t len, const uint8_t* tag) {
    if (k->suite == SUITE_AES_128_GCM_SHA256) return aesgcm_open(&k->gcm, nonce, aad, aadlen, data, len, tag, data);
    return chachapoly_open(k->key, nonce, aad, aadlen, data, len, tag, data);
}

// Each record's nonce: the iv with the record's number XORed into its end (5.3).
static void nonce_for(const keys_t* k, uint8_t nonce[CHACHAPOLY_NONCE_LEN]) {
    memcpy(nonce, k->iv, CHACHAPOLY_NONCE_LEN);
    for (int i = 0; i < 8; i++) nonce[CHACHAPOLY_NONCE_LEN - 1 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

// ---- Records out ----

// Sends data as one or more records of the given type, sealed once
// there is a key. A ChangeCipherSpec always goes in the clear (it means
// nothing in TLS 1.3 and exists for old middleboxes to see).
static int send_record(tls_t* t, uint8_t type, const uint8_t* data, uint32_t len) {
    do {
        uint32_t n = len < MAX_PLAIN ? len : MAX_PLAIN, total;
        uint8_t* o = t->out;
        if (n) memcpy(o + HEADER, data, n);
        o[1] = 3;
        if (t->wr.on && type != REC_CCS) {
            // What is sealed is the data and then its real type; the header says "application data" (5.2).
            o[0] = REC_APPDATA;
            o[2] = 3;
            put16(o + 3, n + 1 + TAG);
            o[HEADER + n] = type;
            uint8_t nonce[CHACHAPOLY_NONCE_LEN];
            nonce_for(&t->wr, nonce);
            seal(&t->wr, nonce, o, HEADER, o + HEADER, n + 1, o + HEADER + n + 1);
            t->wr.seq++;
            total = HEADER + n + 1 + TAG;
        } else {
            o[0] = type;
            o[2] = t->hello_sent ? 3 : 1;  // the first ClientHello says 3.1, for servers that mind (5.1)
            put16(o + 3, n);
            total = HEADER + n;
        }
        t->hello_sent = 1;
        int r = t->io.write(t->io.ctx, o, total);
        if (r < 0) return r;
        data += n;
        len -= n;
    } while (len);
    return 0;
}

static void send_alert(tls_t* t, int alert) {
    if (!t->hello_sent || t->sent_close) return;
    uint8_t a[2] = { alert == AL_CLOSE_NOTIFY ? 1 : 2, (uint8_t)alert };  // warning or fatal
    send_record(t, REC_ALERT, a, 2);
}

// The connection is over: tell the server why (if there is an alert for
// it) and remember the error for any later call.
static int fail(tls_t* t, int err, int alert) {
    if (t->state != ST_DEAD) {
        if (alert != NO_ALERT) send_alert(t, alert);
        t->state = ST_DEAD;
        t->err = err;
    }
    return t->err;
}

// ---- Records in ----

static int read_full(tls_t* t, uint8_t* p, uint32_t n) {
    while (n) {
        int r = t->io.read(t->io.ctx, p, n);
        if (r < 0) return r;
        if (r == 0 || (uint32_t)r > n) return TLS_E_EOF;
        p += r;
        n -= (uint32_t)r;
    }
    return 0;
}

// Reads the next record, opens it if there is a key, and gives its real
// type and the length of its contents, which are at t->in + HEADER.
static int read_record(tls_t* t, uint8_t* type, uint32_t* len) {
    for (;;) {
        uint8_t* h = t->in;
        int r = read_full(t, h, HEADER);
        if (r) return fail(t, r, NO_ALERT);
        uint8_t outer = h[0];
        uint32_t n = (uint32_t)h[3] << 8 | h[4];
        if (outer < REC_CCS || outer > REC_APPDATA || (!t->got_record && h[1] != 3)) {
            // Before anything has made sense, this is most likely not a TLS server at all.
            if (!t->got_record) return fail(t, TLS_E_NOT_TLS, NO_ALERT);
            return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        }
        t->got_record = 1;
        if (n > (t->rd.on && outer == REC_APPDATA ? MAX_CIPHER : MAX_PLAIN))
            return fail(t, TLS_E_TOO_BIG, AL_RECORD_OVERFLOW);
        if ((r = read_full(t, h + HEADER, n)) != 0) return fail(t, r, NO_ALERT);

        if (outer == REC_CCS) {
            // Servers send one for the sake of old middleboxes. During the
            // handshake a single byte 1 is dropped; anything else is an error (5).
            if (!t->ccs_ok || n != 1 || h[HEADER] != 1) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            continue;
        }
        if (!t->rd.on) {
            if (outer == REC_APPDATA) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            *type = outer;
            *len = n;
            return 0;
        }
        // With a key in place nothing but sealed records may come.
        if (outer != REC_APPDATA) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        if (n < TAG) return fail(t, TLS_E_BAD_MAC, AL_BAD_RECORD_MAC);
        uint8_t nonce[CHACHAPOLY_NONCE_LEN];
        nonce_for(&t->rd, nonce);
        uint32_t m = n - TAG;
        if (open_(&t->rd, nonce, h, HEADER, h + HEADER, m, h + HEADER + m))
            return fail(t, TLS_E_BAD_MAC, AL_BAD_RECORD_MAC);
        t->rd.seq++;
        // Inside: the contents, the real type, then any number of zeros as
        // padding. All of it together has a limit of its own (5.4).
        if (m > MAX_PLAIN + 1) return fail(t, TLS_E_TOO_BIG, AL_RECORD_OVERFLOW);
        while (m && h[HEADER + m - 1] == 0) m--;
        if (!m) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        *type = h[HEADER + --m];
        if (*type < REC_ALERT || *type > REC_APPDATA) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        *len = m;
        return 0;
    }
}

// An alert record. 1: nothing to act on, read on. 0: the server said
// goodbye. Otherwise the connection is dead and this is the error.
static int got_alert(tls_t* t, uint32_t len) {
    const uint8_t* a = t->in + HEADER;
    if (len != 2) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
    if (a[1] == AL_CLOSE_NOTIFY) return 0;
    if (a[1] == AL_USER_CANCELED) return 1;  // a close_notify follows (6.1)
    return fail(t, TLS_E_ALERT - a[1], NO_ALERT);  // every other alert is fatal, whatever level it claims (6)
}

// ---- Handshake messages in ----

// Adds the record just read to the handshake bytes waiting.
static int hs_append(tls_t* t, uint32_t len) {
    if (t->hs_at == t->hs_len) t->hs_at = t->hs_len = 0;
    if (t->hs_at) {
        memmove(t->hs, t->hs + t->hs_at, t->hs_len - t->hs_at);
        t->hs_len -= t->hs_at;
        t->hs_at = 0;
    }
    if (t->hs_len + len > t->hs_cap) {
        uint32_t cap = (t->hs_len + len) * 2;
        uint8_t* p = realloc(t->hs, cap);
        if (!p) return fail(t, TLS_E_NOMEM, AL_INTERNAL_ERROR);
        t->hs = p;
        t->hs_cap = cap;
    }
    memcpy(t->hs + t->hs_len, t->in + HEADER, len);
    t->hs_len += len;
    return 0;
}

// 1 with the next handshake message if all of it is here, 0 if not yet.
static int hs_peek(tls_t* t, uint8_t* type, const uint8_t** body, uint32_t* len) {
    uint32_t have = t->hs_len - t->hs_at;
    if (have < 4) return 0;
    const uint8_t* p = t->hs + t->hs_at;
    uint32_t n = (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    if (n > MAX_HANDSHAKE) return fail(t, TLS_E_TOO_BIG, AL_ILLEGAL_PARAMETER);
    if (have < 4 + n) return 0;
    *type = p[0];
    *body = p + 4;
    *len = n;
    return 1;
}

// Moves past the message hs_peek gave, adding it to the transcript if asked.
static void hs_done(tls_t* t, int hash) {
    const uint8_t* p = t->hs + t->hs_at;
    uint32_t n = 4 + ((uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
    if (hash) sha256_update(&t->transcript, p, n);
    t->hs_at += n;
}

// During the handshake: the next message, whatever it is.
static int next_message(tls_t* t, uint8_t* type, const uint8_t** body, uint32_t* len) {
    for (;;) {
        int r = hs_peek(t, type, body, len);
        if (r) return r < 0 ? r : 0;
        uint8_t rt;
        uint32_t rl;
        if ((r = read_record(t, &rt, &rl)) != 0) return r;
        if (rt == REC_ALERT) {
            // (An alert in the middle of a message is no different from one between two.)
            if ((r = got_alert(t, rl)) < 0) return r;
            if (r == 0) return fail(t, TLS_E_EOF, NO_ALERT);  // goodbye, before the handshake was done
            continue;
        }
        // No empty handshake records, and nothing else until the handshake is over (5.1).
        if (rt != REC_HANDSHAKE || rl == 0) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        if ((r = hs_append(t, rl)) != 0) return r;
    }
}

// The next message, which must be of type `want`.
static int get_message(tls_t* t, uint8_t want, const uint8_t** body, uint32_t* len) {
    uint8_t type;
    int r = next_message(t, &type, body, len);
    if (r) return r;
    return type == want ? 0 : fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
}

// Keys change only between records: a message that brings new keys must
// be the last thing in its record (5.1).
static int at_record_end(tls_t* t) {
    return t->hs_at == t->hs_len ? 0 : fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
}

// A cursor over a message being parsed. Reading past the end sets bad
// and gives zeros; the caller checks bad once at the end.
typedef struct {
    const uint8_t* p;
    uint32_t n;
    int bad;
} cur_t;

static const uint8_t* take(cur_t* c, uint32_t n) {
    static const uint8_t zeros[32];
    if (c->bad || c->n < n) {
        c->bad = 1;
        c->n = 0;
        return n <= sizeof zeros ? zeros : 0;
    }
    const uint8_t* p = c->p;
    c->p += n;
    c->n -= n;
    return p;
}
static uint32_t take8(cur_t* c) { return take(c, 1)[0]; }
static uint32_t take16(cur_t* c) { const uint8_t* p = take(c, 2); return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t take24(cur_t* c) { const uint8_t* p = take(c, 3); return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
// The next n bytes as a cursor of their own.
static cur_t sub(cur_t* c, uint32_t n) {
    cur_t s = { 0, 0, 0 };
    if (c->bad || c->n < n) {
        c->bad = 1;
        c->n = 0;
        s.bad = 1;
        return s;
    }
    s.p = c->p;
    s.n = n;
    c->p += n;
    c->n -= n;
    return s;
}

// ---- The handshake ----

// Signature schemes the server may sign with (4.2.3): the ones the
// verifier is to check, which between them cover the web. The same list
// says what may sign the certificates, hence PKCS#1, which TLS 1.3 allows
// there and nowhere else (4.4.3).
static const uint16_t sig_schemes[] = {
    0x0403, 0x0503,          // ECDSA with P-256 and SHA-256, P-384 and SHA-384
    0x0804, 0x0805, 0x0806,  // RSA-PSS (with an ordinary RSA key) and SHA-256, -384, -512
    0x0401, 0x0501, 0x0601,  // RSA PKCS#1 v1.5 and SHA-256, -384, -512: certificates only
};
#define N_SIGS (sizeof sig_schemes / sizeof *sig_schemes)

// Whether the server may sign the handshake with this scheme: one we
// offered, and not PKCS#1.
static int handshake_scheme_ok(uint32_t scheme) {
    if ((scheme & 0xff) == 0x01) return 0;
    for (uint32_t i = 0; i < N_SIGS; i++)
        if (sig_schemes[i] == scheme) return 1;
    return 0;
}

static int send_handshake(tls_t* t, const uint8_t* msg, uint32_t len) {
    sha256_update(&t->transcript, msg, len);
    int r = send_record(t, REC_HANDSHAKE, msg, len);
    return r ? fail(t, r, NO_ALERT) : 0;
}

// ClientHello (4.1.2). The second one, after a HelloRetryRequest, is
// the first again with the server's cookie added.
static int send_client_hello(tls_t* t, const uint8_t* cookie, uint32_t cookie_len) {
    uint32_t name_len = (uint32_t)strlen(t->name);
    uint8_t* m = malloc(512 + name_len + cookie_len);
    if (!m) return fail(t, TLS_E_NOMEM, NO_ALERT);
    uint8_t* p = m + 4;
    put16(p, 0x0303);  // "TLS 1.2": the real version is in an extension (4.2.1)
    p += 2;
    memcpy(p, t->random, 32);
    p += 32;
    *p++ = 32;  // a session id nobody uses, so the handshake looks like a TLS 1.2 one to middleboxes (D.4)
    memcpy(p, t->session_id, 32);
    p += 32;
    put16(p, 4);  // ChaCha20 first: it is ten times faster here, and as safe
    put16(p + 2, SUITE_CHACHA20_POLY1305_SHA256);
    put16(p + 4, SUITE_AES_128_GCM_SHA256);
    p += 6;
    *p++ = 1;  // one compression method: none
    *p++ = 0;
    uint8_t* exts = p;
    p += 2;
    if (name_len) {
        put16(p, EXT_SERVER_NAME);
        put16(p + 2, name_len + 5);
        put16(p + 4, name_len + 3);
        p[6] = 0;  // a host name
        put16(p + 7, name_len);
        memcpy(p + 9, t->name, name_len);
        p += 9 + name_len;
    }
    put16(p, EXT_SUPPORTED_VERSIONS);
    put16(p + 2, 3);
    p[4] = 2;
    put16(p + 5, TLS13);
    p += 7;
    put16(p, EXT_SUPPORTED_GROUPS);
    put16(p + 2, 4);
    put16(p + 4, 2);
    put16(p + 6, GROUP_X25519);
    p += 8;
    put16(p, EXT_SIGNATURE_ALGORITHMS);
    put16(p + 2, 2 + 2 * N_SIGS);
    put16(p + 4, 2 * N_SIGS);
    p += 6;
    for (uint32_t i = 0; i < N_SIGS; i++, p += 2) put16(p, sig_schemes[i]);
    put16(p, EXT_KEY_SHARE);
    put16(p + 2, 2 + 4 + X25519_LEN);
    put16(p + 4, 4 + X25519_LEN);
    put16(p + 6, GROUP_X25519);
    put16(p + 8, X25519_LEN);
    memcpy(p + 10, t->x_public, X25519_LEN);
    p += 10 + X25519_LEN;
    if (cookie_len) {
        put16(p, EXT_COOKIE);
        put16(p + 2, cookie_len + 2);
        put16(p + 4, cookie_len);
        memcpy(p + 6, cookie, cookie_len);
        p += 6 + cookie_len;
    }
    put16(exts, (uint32_t)(p - exts - 2));
    m[0] = HS_CLIENT_HELLO;
    put24(m + 1, (uint32_t)(p - m - 4));
    int r = send_handshake(t, m, (uint32_t)(p - m));
    free(m);
    return r;
}

static int send_ccs(tls_t* t) {
    static const uint8_t one = 1;
    if (t->sent_ccs) return 0;
    t->sent_ccs = 1;
    int r = send_record(t, REC_CCS, &one, 1);
    return r ? fail(t, r, NO_ALERT) : 0;
}

// The random field that marks a ServerHello as a HelloRetryRequest
// (4.1.3): SHA-256 of "HelloRetryRequest".
static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

// ServerHello (4.1.3), or first a HelloRetryRequest (4.1.4) and then
// ServerHello. On return the handshake keys are in place both ways.
static int do_server_hello(tls_t* t) {
    for (int retried = 0;; retried = 1) {
        const uint8_t* body;
        uint32_t len;
        int r = get_message(t, HS_SERVER_HELLO, &body, &len);
        if (r) return r;
        cur_t c = { body, len, 0 };
        uint32_t legacy_version = take16(&c);
        const uint8_t* random = take(&c, 32);
        uint32_t sid_len = take8(&c);
        cur_t sid = sub(&c, sid_len);
        uint32_t suite = take16(&c);
        uint32_t compression = take8(&c);
        if (c.bad) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
        int hrr = memcmp(random, hrr_random, 32) == 0;

        // The extensions. A server older than TLS 1.3 may send none at all.
        int have_version = 0, have_share = 0, unknown = 0;
        uint32_t version = 0, group = 0;
        cur_t share = { 0, 0, 0 }, cookie = { 0, 0, 0 };
        if (c.n) {
            cur_t exts = sub(&c, take16(&c));
            while (exts.n && !exts.bad) {
                uint32_t type = take16(&exts);
                cur_t e = sub(&exts, take16(&exts));
                if (exts.bad) break;
                if (type == EXT_SUPPORTED_VERSIONS) {
                    if (have_version) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
                    have_version = 1;
                    version = take16(&e);
                } else if (type == EXT_KEY_SHARE) {
                    if (have_share) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
                    have_share = 1;
                    group = take16(&e);
                    if (!hrr) share = sub(&e, take16(&e));
                } else if (type == EXT_COOKIE && hrr) {
                    if (cookie.p) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
                    cookie = sub(&e, take16(&e));
                    if (!cookie.n) e.bad = 1;
                } else {
                    unknown = 1;
                    continue;
                }
                if (e.bad || e.n) exts.bad = 1;
            }
            if (c.bad || c.n || exts.bad) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
        }
        // No supported_versions: the server took us for a TLS 1.2 client
        // and answered as one (4.2.1). We do not speak that.
        if (!have_version) return fail(t, TLS_E_VERSION, AL_PROTOCOL_VERSION);
        if (version != TLS13 || legacy_version != 0x0303) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
        // An extension we did not ask for (4.2).
        if (unknown) return fail(t, TLS_E_PARAM, AL_UNSUPPORTED_EXTENSION);
        // Our session id back, a cipher suite we offered (after a retry,
        // the one the retry named: 4.1.4), no compression.
        if (sid.n != 32 || memcmp(sid.p, t->session_id, 32) != 0 ||
            (suite != SUITE_CHACHA20_POLY1305_SHA256 && suite != SUITE_AES_128_GCM_SHA256) ||
            (retried && suite != t->suite) || compression != 0)
            return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
        t->suite = suite;

        if (hrr) {
            // A retry must ask for something to be different (4.1.4). The
            // one group we offered is in the first hello already, so all
            // it can ask of us is to send a cookie back. And only once.
            if (retried) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            if (have_share || !cookie.p) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
            // The transcript starts over with a stand-in for the first
            // hello: a made-up message holding its hash (4.4.1).
            uint8_t stand_in[4 + HASH] = { 254, 0, 0, HASH };
            transcript_hash(t, stand_in + 4);
            sha256_init(&t->transcript);
            sha256_update(&t->transcript, stand_in, sizeof stand_in);
            uint8_t* kept = malloc(cookie.n);
            if (!kept) return fail(t, TLS_E_NOMEM, AL_INTERNAL_ERROR);
            memcpy(kept, cookie.p, cookie.n);
            hs_done(t, 1);
            if ((r = at_record_end(t)) == 0 && (r = send_ccs(t)) == 0) r = send_client_hello(t, kept, cookie.n);
            free(kept);
            if (r) return r;
            continue;
        }
        if (!have_share) return fail(t, TLS_E_PARAM, AL_MISSING_EXTENSION);
        if (group != GROUP_X25519 || share.n != X25519_LEN) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
        uint8_t shared[X25519_LEN];
        // All zeros: the server sent a value that gives the same result
        // for any secret of ours. Not a key to use (7.4.2).
        if (x25519(shared, t->x_secret, share.p)) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
        hs_done(t, 1);
        if ((r = at_record_end(t)) != 0) return r;

        // The key schedule (7.1), as far as the handshake keys. With no
        // pre-shared key the early secret is made from zeros.
        static const uint8_t zeros[HASH];
        uint8_t early[HASH], derived[HASH], handshake[HASH], empty_hash[HASH], secret[HASH];
        sha256("", 0, empty_hash);
        hkdf_sha256_extract(0, 0, zeros, HASH, early);
        tls13_expand_label(early, "derived", empty_hash, HASH, derived, HASH);
        hkdf_sha256_extract(derived, HASH, shared, sizeof shared, handshake);
        derive_secret(t, handshake, "s hs traffic", secret);
        set_keys(&t->rd, t->suite, secret);
        derive_secret(t, handshake, "c hs traffic", secret);
        set_keys(&t->wr, t->suite, secret);
        tls13_expand_label(handshake, "derived", empty_hash, HASH, derived, HASH);
        hkdf_sha256_extract(derived, HASH, zeros, HASH, t->master);
        ct_wipe(shared, sizeof shared);
        ct_wipe(early, sizeof early);
        ct_wipe(derived, sizeof derived);
        ct_wipe(handshake, sizeof handshake);
        ct_wipe(secret, sizeof secret);
        ct_wipe(t->x_secret, sizeof t->x_secret);
        return 0;
    }
}

// A list of extensions that must only parse: each a type and a length.
// Returns -1 if it does not; otherwise `seen` gets a bit for each type
// under 64 and the result is 1 if a type was there twice, else 0.
static int scan_extensions(cur_t* c, uint64_t* seen, uint64_t* big) {
    int twice = 0;
    cur_t exts = sub(c, take16(c));
    *seen = *big = 0;
    while (exts.n && !exts.bad) {
        uint32_t type = take16(&exts);
        sub(&exts, take16(&exts));
        if (exts.bad) break;
        if (type < 64) {
            if (*seen >> type & 1) twice = 1;
            *seen |= (uint64_t)1 << type;
        } else {
            (*big)++;
        }
    }
    return exts.bad || c->bad ? -1 : twice;
}

// Hands the verifier the chain and the signature, and ends the handshake
// with the matching alert if it says no. Anything but a clear yes is a no.
static int ask_verifier(tls_t* t, uint32_t scheme, const uint8_t* signature, uint32_t signature_len) {
    // The chain: each CertificateEntry's cert_data. Its shape was checked when it came.
    cur_t c = { t->certs, t->certs_len, 0 };
    take(&c, take8(&c));
    cur_t list = sub(&c, take24(&c)), count = list;
    uint32_t n = 0;
    while (count.n && !count.bad) {
        sub(&count, take24(&count));
        sub(&count, take16(&count));
        n++;
    }
    const uint8_t** cert = malloc(n * sizeof *cert);
    uint32_t* cert_len = malloc(n * sizeof *cert_len);
    if (!cert || !cert_len) {
        free(cert);
        free(cert_len);
        return fail(t, TLS_E_NOMEM, AL_INTERNAL_ERROR);
    }
    for (uint32_t i = 0; i < n; i++) {
        cur_t one = sub(&list, take24(&list));
        cert[i] = one.p;
        cert_len[i] = one.n;
        sub(&list, take16(&list));
    }

    // 64 spaces, the context string, a zero byte, the transcript hash (4.4.3).
    static const char context[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + sizeof context + HASH];
    memset(content, ' ', 64);
    memcpy(content + 64, context, sizeof context);  // (with its terminating zero)
    transcript_hash(t, content + 64 + sizeof context);

    tls_peer_t peer = { t->asked, n, cert, cert_len, (uint16_t)scheme, content, sizeof content, signature, signature_len };
    int v = t->verify(t->verify_ctx, &peer);
    free(cert);
    free(cert_len);
    ct_wipe(t->certs, t->certs_len);
    free(t->certs);
    t->certs = 0;
    t->certs_len = 0;
    switch (v) {
    case 0: t->verified = 1; return 0;
    case TLS_E_CERT_BAD:
    case TLS_E_CERT_NAME: return fail(t, v, AL_BAD_CERTIFICATE);
    case TLS_E_CERT_UNSUPPORTED: return fail(t, v, AL_UNSUPPORTED_CERTIFICATE);
    case TLS_E_CERT_EXPIRED: return fail(t, v, AL_CERTIFICATE_EXPIRED);
    case TLS_E_CERT_UNKNOWN_CA: return fail(t, v, AL_UNKNOWN_CA);
    case TLS_E_CERT_SIGNATURE: return fail(t, v, AL_DECRYPT_ERROR);
    }
    return v < 0 ? fail(t, v, AL_INTERNAL_ERROR) : fail(t, TLS_E_CERT_BAD, AL_BAD_CERTIFICATE);
}

// The rest of the server's flight, all under the handshake keys:
// EncryptedExtensions, perhaps CertificateRequest, Certificate,
// CertificateVerify, Finished (4.3, 4.4).
static int do_server_flight(tls_t* t, int* cert_requested) {
    const uint8_t* body = 0;
    uint32_t len = 0;
    uint64_t seen, big;
    int r = get_message(t, HS_ENCRYPTED_EXTENSIONS, &body, &len);
    if (r) return r;
    cur_t c = { body, len, 0 };
    r = scan_extensions(&c, &seen, &big);
    if (r < 0 || c.n) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
    if (r) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
    // Of what we sent, only these two may be answered here (4.2); an
    // answer to anything we did not send is an error.
    if (big || (seen & ~((uint64_t)1 << EXT_SERVER_NAME | (uint64_t)1 << EXT_SUPPORTED_GROUPS)))
        return fail(t, TLS_E_PARAM, AL_UNSUPPORTED_EXTENSION);
    hs_done(t, 1);

    // The server may ask for a certificate of ours. We have none, and say so later.
    uint8_t type = 0;
    *cert_requested = 0;
    if ((r = next_message(t, &type, &body, &len)) != 0) return r;
    if (type == HS_CERTIFICATE_REQUEST) {
        c = (cur_t){ body, len, 0 };
        uint32_t context_len = take8(&c);
        take(&c, context_len);
        r = scan_extensions(&c, &seen, &big);
        if (r < 0 || c.n) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
        if (r || context_len) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);  // the context is empty in a handshake (4.3.2)
        if (!(seen >> EXT_SIGNATURE_ALGORITHMS & 1)) return fail(t, TLS_E_PARAM, AL_MISSING_EXTENSION);
        *cert_requested = 1;
        hs_done(t, 1);
    }

    // The certificate chain (4.4.2). Its shape is checked; what it says
    // is not: see tls.h.
    if ((r = get_message(t, HS_CERTIFICATE, &body, &len)) != 0) return r;
    c = (cur_t){ body, len, 0 };
    uint32_t context_len = take8(&c);
    take(&c, context_len);
    cur_t list = sub(&c, take24(&c));
    uint32_t certs = 0;
    while (list.n && !list.bad) {
        cur_t cert = sub(&list, take24(&list));
        if (!cert.n) list.bad = 1;
        uint64_t s, b;
        if (scan_extensions(&list, &s, &b) < 0) list.bad = 1;
        certs++;
    }
    // (No certificate at all is a decode_error too: 4.4.2.4.)
    if (c.bad || c.n || list.bad || !certs) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
    if (context_len) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
    if (t->verify) {
        // The message moves on in hs once used; the verifier needs it until CertificateVerify.
        if (!(t->certs = malloc(len))) return fail(t, TLS_E_NOMEM, AL_INTERNAL_ERROR);
        memcpy(t->certs, body, len);
        t->certs_len = len;
    }
    hs_done(t, 1);

    // The server's signature over the handshake so far (4.4.3), with the
    // key in its certificate. What it signed is built here, so that a
    // verifier given it checks the signature over this handshake and no
    // other.
    if ((r = get_message(t, HS_CERTIFICATE_VERIFY, &body, &len)) != 0) return r;
    c = (cur_t){ body, len, 0 };
    uint32_t scheme = take16(&c);
    cur_t sig = sub(&c, take16(&c));
    if (c.bad || c.n || !sig.n) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
    if (!handshake_scheme_ok(scheme)) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
    if (t->verify && (r = ask_verifier(t, scheme, sig.p, sig.n)) != 0) return r;
    hs_done(t, 1);

    // Finished (4.4.4): the server's checksum of everything up to here.
    if ((r = get_message(t, HS_FINISHED, &body, &len)) != 0) return r;
    uint8_t want[HASH];
    finished_mac(t, t->rd.secret, want);
    if (len != HASH) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
    if (!ct_equal(body, want, HASH)) return fail(t, TLS_E_FINISHED, AL_DECRYPT_ERROR);
    hs_done(t, 1);
    return at_record_end(t);
}

int tls_connect(tls_t* t, const char* server_name) {
    if (t->state != ST_NEW) return t->state == ST_DEAD ? t->err : TLS_E_CLOSED;
    // The name goes into the hello as it is, so it must be a plain host
    // name. An address is not a name and is left out (RFC 6066).
    uint32_t ip;
    t->name[0] = t->asked[0] = 0;
    if (server_name && parse_ipv4(server_name, &ip) == 0) {
        size_t n = strlen(server_name);
        if (n >= sizeof t->asked) return fail(t, TLS_E_NAME, NO_ALERT);
        memcpy(t->asked, server_name, n + 1);
    } else if (server_name) {
        size_t n = strlen(server_name);
        if (n && server_name[n - 1] == '.') n--;
        if (n == 0 || n > 253) return fail(t, TLS_E_NAME, NO_ALERT);
        for (size_t i = 0; i < n; i++) {
            char ch = server_name[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                  ch == '.' || ch == '_'))
                return fail(t, TLS_E_NAME, NO_ALERT);
        }
        memcpy(t->name, server_name, n);
        t->name[n] = 0;
        memcpy(t->asked, t->name, n + 1);
    }
    uint8_t fresh[32 + 32 + X25519_LEN];
    int r = t->io.random(t->io.ctx, fresh, sizeof fresh);
    if (r < 0) return fail(t, r, NO_ALERT);
    memcpy(t->random, fresh, 32);
    memcpy(t->session_id, fresh + 32, 32);
    memcpy(t->x_secret, fresh + 64, X25519_LEN);
    ct_wipe(fresh, sizeof fresh);
    x25519_public(t->x_public, t->x_secret);
    sha256_init(&t->transcript);

    if ((r = send_client_hello(t, 0, 0)) != 0) return r;
    t->ccs_ok = 1;
    if ((r = do_server_hello(t)) != 0) return r;
    int cert_requested = 0;
    if ((r = do_server_flight(t, &cert_requested)) != 0) return r;
    t->ccs_ok = 0;

    // The application keys come from the handshake up to the server's
    // Finished (7.1). The server's is used from here; ours after our Finished.
    uint8_t client_secret[HASH], server_secret[HASH];
    derive_secret(t, t->master, "c ap traffic", client_secret);
    derive_secret(t, t->master, "s ap traffic", server_secret);
    set_keys(&t->rd, t->suite, server_secret);
    ct_wipe(server_secret, sizeof server_secret);
    ct_wipe(t->master, sizeof t->master);

    // Our flight: a ChangeCipherSpec for old middleboxes (D.4), an empty
    // Certificate if one was asked for (4.4.2.4), and Finished.
    if ((r = send_ccs(t)) != 0) return r;
    if (cert_requested) {
        static const uint8_t none[] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
        if ((r = send_handshake(t, none, sizeof none)) != 0) return r;
    }
    uint8_t fin[4 + HASH] = { HS_FINISHED, 0, 0, HASH };
    finished_mac(t, t->wr.secret, fin + 4);
    if ((r = send_handshake(t, fin, sizeof fin)) != 0) return r;
    set_keys(&t->wr, t->suite, client_secret);
    ct_wipe(client_secret, sizeof client_secret);
    t->state = ST_OPEN;
    return 0;
}

// ---- After the handshake ----

// The next traffic secret from the current one (7.2).
static void update_keys(keys_t* k) {
    uint8_t next[HASH];
    tls13_expand_label(k->secret, "traffic upd", 0, 0, next, HASH);
    set_keys(k, k->suite, next);
    ct_wipe(next, sizeof next);
}

// Handshake messages that come once the connection is up (4.6).
static int post_handshake(tls_t* t) {
    for (;;) {
        uint8_t type = 0;
        const uint8_t* body = 0;
        uint32_t len = 0;
        int r = hs_peek(t, &type, &body, &len);
        if (r <= 0) return r;
        if (type == HS_NEW_SESSION_TICKET) {
            hs_done(t, 0);  // an offer to resume later; not taken up
        } else if (type == HS_KEY_UPDATE) {
            // The server moves to its next key, and may ask us to do the same (4.6.3).
            if (len != 1) return fail(t, TLS_E_DECODE, AL_DECODE_ERROR);
            int asked = body[0];
            if (asked > 1) return fail(t, TLS_E_PARAM, AL_ILLEGAL_PARAMETER);
            hs_done(t, 0);
            if ((r = at_record_end(t)) != 0) return r;
            update_keys(&t->rd);
            if (asked && !t->sent_close) {
                static const uint8_t update[] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if ((r = send_record(t, REC_HANDSHAKE, update, sizeof update)) != 0) return fail(t, r, NO_ALERT);
                update_keys(&t->wr);
            }
        } else {
            // Among the rest, a late CertificateRequest, which we did not say we would take (4.6.2).
            return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        }
    }
}

int tls_read(tls_t* t, void* buf, uint32_t n) {
    for (;;) {
        if (t->app_len) {
            uint32_t take_n = n < t->app_len ? n : t->app_len;
            memcpy(buf, t->in + t->app_at, take_n);
            t->app_at += take_n;
            t->app_len -= take_n;
            return (int)take_n;
        }
        if (t->state == ST_EOF) return 0;
        if (t->state != ST_OPEN) return t->state == ST_DEAD ? t->err : TLS_E_CLOSED;
        if (!n) return 0;
        uint8_t type;
        uint32_t len;
        int r = read_record(t, &type, &len);
        if (r) return r;
        // A handshake message begun must be finished before anything else comes (5.1).
        if (type != REC_HANDSHAKE && t->hs_at != t->hs_len) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        if (type == REC_APPDATA) {
            t->app_at = HEADER;
            t->app_len = len;
        } else if (type == REC_ALERT) {
            if ((r = got_alert(t, len)) < 0) return r;
            if (r == 0) t->state = ST_EOF;
        } else {
            if (len == 0) return fail(t, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            if ((r = hs_append(t, len)) != 0 || (r = post_handshake(t)) != 0) return r;
        }
    }
}

int tls_write(tls_t* t, const void* buf, uint32_t n) {
    if (t->state == ST_DEAD) return t->err;
    if ((t->state != ST_OPEN && t->state != ST_EOF) || t->sent_close) return TLS_E_CLOSED;
    if (!n) return 0;
    int r = send_record(t, REC_APPDATA, buf, n);
    return r ? fail(t, r, NO_ALERT) : (int)n;
}

int tls_close(tls_t* t) {
    if (t->state == ST_DEAD) return t->err;
    if ((t->state != ST_OPEN && t->state != ST_EOF) || t->sent_close) return TLS_E_CLOSED;
    uint8_t a[2] = { 1, AL_CLOSE_NOTIFY };
    int r = send_record(t, REC_ALERT, a, 2);
    t->sent_close = 1;
    return r ? fail(t, r, NO_ALERT) : 0;
}

void tls_set_verify(tls_t* t, tls_verify_fn fn, void* ctx) {
    if (t->state != ST_NEW) return;
    t->verify = fn;
    t->verify_ctx = ctx;
}

int tls_peer_verified(const tls_t* t) { return t->verified && (t->state == ST_OPEN || t->state == ST_EOF); }

tls_t* tls_new(const tls_io_t* io) {
    tls_t* t = malloc(sizeof *t);
    if (!t) return 0;
    memset(t, 0, sizeof *t);
    t->io = *io;
    t->state = ST_NEW;
    return t;
}

void tls_free(tls_t* t) {
    if (!t) return;
    if (t->hs) {
        ct_wipe(t->hs, t->hs_cap);
        free(t->hs);
    }
    free(t->certs);
    ct_wipe(t, sizeof *t);
    free(t);
}

const char* tls_strerror(int err) {
    static const struct { int code; const char* name; } alerts[] = {
        { 10, "unexpected message" }, { 20, "bad record MAC" }, { 22, "record overflow" },
        { 40, "handshake failure (it may not speak TLS 1.3, or shares no cipher with us)" },
        { 42, "bad certificate" }, { 43, "unsupported certificate" }, { 44, "certificate revoked" },
        { 45, "certificate expired" }, { 46, "certificate unknown" }, { 47, "illegal parameter" },
        { 48, "unknown certificate authority" }, { 49, "access denied" }, { 50, "decode error" },
        { 51, "decrypt error" }, { 70, "protocol version (it does not speak TLS 1.3)" },
        { 71, "insufficient security" }, { 80, "internal error" }, { 86, "inappropriate fallback" },
        { 109, "missing extension" }, { 110, "unsupported extension" }, { 112, "unrecognized name" },
        { 116, "certificate required" }, { 120, "no application protocol" },
    };
    static char buf[112];
    switch (err) {
    case TLS_E_EOF: return "the connection closed in the middle";
    case TLS_E_NOT_TLS: return "the server did not answer with TLS";
    case TLS_E_VERSION: return "the server does not speak TLS 1.3";
    case TLS_E_PROTOCOL: return "the server sent something TLS does not allow there";
    case TLS_E_DECODE: return "the server sent a malformed message";
    case TLS_E_PARAM: return "the server chose something that was not on offer";
    case TLS_E_BAD_MAC: return "a record failed its integrity check (damaged or forged)";
    case TLS_E_FINISHED: return "the handshake's checksum is wrong (someone meddled with it)";
    case TLS_E_TOO_BIG: return "the server sent a record or message over the size limit";
    case TLS_E_NOMEM: return "out of memory";
    case TLS_E_CLOSED: return "the connection is closed";
    case TLS_E_NAME: return "not a host name a certificate could be for";
    case TLS_E_CERT_BAD: return "the server's certificate is not one we can trust";
    case TLS_E_CERT_UNSUPPORTED: return "the server's certificate uses a kind of key or signature we cannot check";
    case TLS_E_CERT_EXPIRED: return "the server's certificate has expired or is not yet valid";
    case TLS_E_CERT_UNKNOWN_CA: return "the server's certificate is not signed by anyone we trust";
    case TLS_E_CERT_NAME: return "the server's certificate is for some other name";
    case TLS_E_CERT_SIGNATURE: return "the server's signature does not match its certificate";
    }
    if (err <= TLS_E_ALERT && err > TLS_E_ALERT - 256) {
        int code = TLS_E_ALERT - err;
        for (uint32_t i = 0; i < sizeof alerts / sizeof *alerts; i++)
            if (alerts[i].code == code) {
                snprintf(buf, sizeof buf, "the server refused: %s", alerts[i].name);
                return buf;
            }
        snprintf(buf, sizeof buf, "the server refused (alert %d)", code);
        return buf;
    }
    return strerror(err);
}

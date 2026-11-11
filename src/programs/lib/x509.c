// Certificates: see x509.h.
#include "x509.h"
#include "der.h"
#include "sig.h"
#include "lib.h"

void* malloc(size_t size);
void* realloc(void* p, size_t size);
void free(void* p);

#define BAD         TLS_E_CERT_BAD
#define UNSUPPORTED TLS_E_CERT_UNSUPPORTED

static int span_eq(x509_span_t a, x509_span_t b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }

static int oid_is(der_t o, const uint8_t* want, uint32_t n) { return o.n == n && memcmp(o.p, want, n) == 0; }

// The next element, with its tag and length: whole is all of it.
static int der_whole(der_t* d, uint8_t tag, x509_span_t* whole, der_t* contents) {
    const uint8_t* start = d->p;
    if (der_expect(d, tag, contents) != 0) return -1;
    whole->p = start;
    whole->n = (uint32_t)(d->p - start);
    return 0;
}

static int fail(der_t* d) {
    d->bad = 1;
    d->n = 0;
    return -1;
}

uint64_t x509_time(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t min, uint32_t sec) {
    return (uint64_t)(year * 10000 + month * 100 + day) << 32 | (hour * 10000 + min * 100 + sec);
}

// ---- Reading a certificate ----

// Object identifiers' contents.
static const uint8_t OID_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const uint8_t OID_RSA_SHA256[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const uint8_t OID_RSA_SHA384[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const uint8_t OID_RSA_SHA512[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const uint8_t OID_EC[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const uint8_t OID_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const uint8_t OID_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const uint8_t OID_P256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_SERVER_AUTH[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
static const uint8_t OID_ANY_EKU[] = { 0x55, 0x1d, 0x25, 0x00 };
static const uint8_t OID_AIA[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x01 };

enum { SIGK_RSA = 1, SIGK_ECDSA };

// AlgorithmIdentifier of a signature: the RSA ones with a NULL
// parameter (RFC 4055), the ECDSA ones with none (RFC 5758).
static void sig_alg(der_t a, int* kind, int* hash) {
    der_t oid, null;
    *kind = *hash = 0;
    if (der_expect(&a, DER_OID, &oid) != 0) return;
    static const struct { const uint8_t* oid; uint32_t n; int kind, hash; } algs[] = {
        { OID_RSA_SHA256, sizeof OID_RSA_SHA256, SIGK_RSA, HASH_SHA256 },
        { OID_RSA_SHA384, sizeof OID_RSA_SHA384, SIGK_RSA, HASH_SHA384 },
        { OID_RSA_SHA512, sizeof OID_RSA_SHA512, SIGK_RSA, HASH_SHA512 },
        { OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256, SIGK_ECDSA, HASH_SHA256 },
        { OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384, SIGK_ECDSA, HASH_SHA384 },
        { OID_ECDSA_SHA512, sizeof OID_ECDSA_SHA512, SIGK_ECDSA, HASH_SHA512 },
    };
    for (uint32_t i = 0; i < sizeof algs / sizeof *algs; i++) {
        if (!oid_is(oid, algs[i].oid, algs[i].n)) continue;
        if (algs[i].kind == SIGK_RSA && (der_expect(&a, DER_NULL, &null) != 0 || null.n)) return;
        if (a.n || a.bad) return;
        *kind = algs[i].kind;
        *hash = algs[i].hash;
    }
}

static int digits(const uint8_t* p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

// UTCTime (YYMMDDhhmmssZ, 1950 to 2049) or GeneralizedTime (YYYYMMDDhhmmssZ), to the second, in UTC.
static int read_time(der_t* d, uint64_t* out) {
    uint8_t tag;
    der_t c;
    if (der_next(d, &tag, &c) != 0) return -1;
    int year, at;
    if (tag == DER_UTC_TIME && c.n == 13) {
        year = digits(c.p, 2);
        year += year < 50 ? 2000 : 1900;
        at = 2;
    } else if (tag == DER_GEN_TIME && c.n == 15) {
        year = digits(c.p, 4);
        at = 4;
    } else return fail(d);
    int mo = digits(c.p + at, 2), day = digits(c.p + at + 2, 2), h = digits(c.p + at + 4, 2),
        mi = digits(c.p + at + 6, 2), s = digits(c.p + at + 8, 2);
    static const uint8_t mdays[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year < 0 || mo < 1 || mo > 12 || day < 1 || day > mdays[mo - 1] || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        s < 0 || s > 59 || c.p[c.n - 1] != 'Z')
        return fail(d);
    if (mo == 2 && day == 29 && (year % 4 || (year % 100 == 0 && year % 400))) return fail(d);
    *out = x509_time((uint32_t)year, (uint32_t)mo, (uint32_t)day, (uint32_t)h, (uint32_t)mi, (uint32_t)s);
    return 0;
}

// A Name: SEQUENCE OF RelativeDistinguishedName (a non-empty SET OF
// SEQUENCE { type OID, value }).
static int check_name(der_t n) {
    while (n.n) {
        der_t rdn, atv, oid, value;
        uint8_t tag;
        if (der_expect(&n, DER_SET, &rdn) != 0 || !rdn.n) return -1;
        while (rdn.n) {
            if (der_expect(&rdn, DER_SEQUENCE, &atv) != 0 || der_expect(&atv, DER_OID, &oid) != 0 ||
                der_next(&atv, &tag, &value) != 0 || atv.n)
                return -1;
        }
        if (rdn.bad) return -1;
    }
    return n.bad ? -1 : 0;
}

// A DNS name as a certificate may hold one: letters, digits and hyphens in
// labels of 1 to 63 bytes, no hyphen at either end of a label; and when
// wild, "*." before at least two labels (so "*.com" is refused).
static int dns_ok(const uint8_t* p, uint32_t n, int wild_ok) {
    uint32_t min_labels = 1;
    if (wild_ok && n >= 2 && p[0] == '*' && p[1] == '.') {
        p += 2;
        n -= 2;
        min_labels = 2;
    }
    if (!n || n > 253) return 0;
    uint32_t labels = 0, len = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '.') {
            if (!len || len > 63 || p[i - 1] == '-' || p[i - len] == '-') return 0;
            labels++;
            len = 0;
            continue;
        }
        uint8_t ch = p[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) return 0;
        len++;
    }
    return labels >= min_labels;
}

// GeneralNames in a subjectAltName: well formed, at least one, DNS names
// and IP addresses as they must be.
static int check_san(der_t s) {
    if (!s.n) return -1;
    while (s.n) {
        uint8_t tag;
        der_t v;
        if (der_next(&s, &tag, &v) != 0) return -1;
        if (tag == DER_IMPLICIT(2) && !dns_ok(v.p, v.n, 1)) return -1;
        if (tag == DER_IMPLICIT(7) && v.n != 4 && v.n != 16) return -1;
        if (tag == DER_CONTEXT(4) && (der_expect(&v, DER_SEQUENCE, &v) != 0 || check_name(v) != 0)) return -1;
    }
    return 0;
}

// The prefix length an IP mask stands for, or -1 if its ones are not all at the front.
static int mask_bits(const uint8_t* m, uint32_t n) {
    int bits = 0;
    uint32_t i = 0;
    for (; i < n && m[i] == 0xff; i++) bits += 8;
    if (i < n) {
        uint8_t b = m[i];
        while (b & 0x80) {
            bits++;
            b = (uint8_t)(b << 1);
        }
        if (b) return -1;
        for (i++; i < n; i++)
            if (m[i]) return -1;
    }
    return bits;
}

// GeneralSubtrees: each a SEQUENCE { base GeneralName } (no minimum or
// maximum), its DNS names, addresses and directory names well formed.
static int check_subtrees(der_t s) {
    if (!s.n) return -1;
    while (s.n) {
        der_t st, v;
        uint8_t tag;
        if (der_expect(&s, DER_SEQUENCE, &st) != 0 || der_next(&st, &tag, &v) != 0 || st.n) return -1;
        if (tag == DER_IMPLICIT(2) && v.n && !dns_ok(v.p, v.n, 0)) return -1;
        if (tag == DER_IMPLICIT(7)) {
            if (v.n != 8 && v.n != 32) return -1;
            uint32_t half = v.n / 2;
            if (mask_bits(v.p + half, half) < 0) return -1;
            for (uint32_t i = 0; i < half; i++)
                if (v.p[i] & ~v.p[half + i]) return -1;
        }
        if (tag == DER_CONTEXT(4) && (der_expect(&v, DER_SEQUENCE, &v) != 0 || check_name(v) != 0)) return -1;
    }
    return 0;
}

// Extension OIDs under id-ce (2.5.29): 55 1d xx.
enum { CE_SKI = 14, CE_KU = 15, CE_SAN = 17, CE_BC = 19, CE_NC = 30, CE_POLICIES = 32, CE_POLICY_MAPPINGS = 33,
       CE_AKI = 35, CE_POLICY_CONSTRAINTS = 36, CE_EKU = 37, CE_INHIBIT_ANY = 54 };

static int read_extension(x509_t* c, der_t oid, int critical, der_t v) {
    if (oid_is(oid, OID_AIA, sizeof OID_AIA)) {
        // Authority information access: SEQUENCE OF SEQUENCE { method, location }; never critical.
        der_t seq, ad, m;
        uint8_t tag;
        if (critical || der_expect(&v, DER_SEQUENCE, &seq) != 0 || v.n || !seq.n) return BAD;
        while (seq.n)
            if (der_expect(&seq, DER_SEQUENCE, &ad) != 0 || der_expect(&ad, DER_OID, &m) != 0 ||
                der_next(&ad, &tag, &m) != 0 || ad.n)
                return BAD;
        return 0;
    }
    if (oid.n != 3 || oid.p[0] != 0x55 || oid.p[1] != 0x1d) return critical ? UNSUPPORTED : 0;
    der_t seq, x;
    switch (oid.p[2]) {
    case CE_KU: {
        // A BIT STRING of at least one bit.
        if (der_expect(&v, DER_BIT_STRING, &x) != 0 || v.n || x.n < 2 || x.n > 3 || x.p[0] > 7) return BAD;
        c->has_ku = 1;
        for (uint32_t i = 0; i < 8 * (x.n - 1) - x.p[0]; i++)
            if (x.p[1 + i / 8] >> (7 - i % 8) & 1) c->ku |= 1u << i;
        return c->ku ? 0 : BAD;
    }
    case CE_BC: {
        // SEQUENCE { cA BOOLEAN DEFAULT FALSE, pathLenConstraint INTEGER OPTIONAL }
        if (der_expect(&v, DER_SEQUENCE, &seq) != 0 || v.n) return BAD;
        c->has_bc = 1;
        c->bc_critical = critical;
        if (der_peek(&seq, DER_BOOLEAN)) {
            if (der_expect(&seq, DER_BOOLEAN, &x) != 0 || x.n != 1 || x.p[0] != 0xff) return BAD;  // FALSE is left out
            c->is_ca = 1;
        }
        if (seq.n) {
            const uint8_t* p;
            uint32_t n;
            if (der_uint(&seq, &p, &n) != 0 || seq.n || n > 1) return BAD;
            if (c->is_ca) c->path_len = p[0];  // without cA it means nothing, and is ignored
        }
        return 0;
    }
    case CE_EKU:
        if (der_expect(&v, DER_SEQUENCE, &seq) != 0 || v.n || !seq.n) return BAD;
        c->has_eku = 1;
        while (seq.n) {
            if (der_expect(&seq, DER_OID, &x) != 0) return BAD;
            if (oid_is(x, OID_SERVER_AUTH, sizeof OID_SERVER_AUTH)) c->eku_server_auth = 1;
            if (oid_is(x, OID_ANY_EKU, sizeof OID_ANY_EKU)) c->eku_any = 1;
        }
        return 0;
    case CE_SAN:
        if (der_expect(&v, DER_SEQUENCE, &seq) != 0 || v.n || check_san(seq) != 0) return BAD;
        c->has_san = 1;
        c->san_critical = critical;
        c->san = (x509_span_t){ seq.p, seq.n };
        return 0;
    case CE_NC:
        // SEQUENCE { permitted [0] OPTIONAL, excluded [1] OPTIONAL }, not both absent.
        if (der_expect(&v, DER_SEQUENCE, &seq) != 0 || v.n || !seq.n) return BAD;
        c->has_nc = 1;
        if (der_peek(&seq, DER_CONTEXT(0))) {
            if (der_expect(&seq, DER_CONTEXT(0), &x) != 0 || check_subtrees(x) != 0) return BAD;
            c->nc_permitted = (x509_span_t){ x.p, x.n };
        }
        if (der_peek(&seq, DER_CONTEXT(1))) {
            if (der_expect(&seq, DER_CONTEXT(1), &x) != 0 || check_subtrees(x) != 0) return BAD;
            c->nc_excluded = (x509_span_t){ x.p, x.n };
        }
        return seq.n || seq.bad ? BAD : 0;
    case CE_AKI:
    case CE_SKI:
        return critical ? BAD : 0;  // RFC 5280 4.2.1.1, 4.2.1.2: never critical
    case CE_POLICIES:
    case CE_POLICY_MAPPINGS:
    case CE_POLICY_CONSTRAINTS:
    case CE_INHIBIT_ANY:
        return 0;  // policies: understood well enough to know the Web PKI does not use them
    default:
        return critical ? UNSUPPORTED : 0;
    }
}

int x509_parse(x509_t* c, const uint8_t* der, uint32_t len) {
    memset(c, 0, sizeof *c);
    c->path_len = -1;
    c->der = (x509_span_t){ der, len };
    der_t d = der_of(der, len), cert, tbs, alg, alg2, v, x, spki, key_alg, oid;
    x509_span_t alg_whole, alg2_whole;
    if (der_expect(&d, DER_SEQUENCE, &cert) != 0 || d.n) return BAD;
    if (der_whole(&cert, DER_SEQUENCE, &c->tbs, &tbs) != 0 || der_whole(&cert, DER_SEQUENCE, &alg_whole, &alg) != 0 ||
        der_expect(&cert, DER_BIT_STRING, &x) != 0 || cert.n || !x.n || x.p[0])
        return BAD;
    c->sig = (x509_span_t){ x.p + 1, x.n - 1 };
    sig_alg(alg, &c->sig_kind, &c->sig_hash);

    // Version: [0] EXPLICIT INTEGER, 2 (v3) and nothing else.
    if (!der_peek(&tbs, DER_CONTEXT(0))) return UNSUPPORTED;
    if (der_expect(&tbs, DER_CONTEXT(0), &v) != 0 || der_expect(&v, DER_INTEGER, &x) != 0 || v.n) return BAD;
    if (x.n != 1 || x.p[0] != 2) return UNSUPPORTED;
    // Serial: any minimal INTEGER here; whether it is positive is for the path.
    if (der_expect(&tbs, DER_INTEGER, &x) != 0 || !x.n || (x.n > 1 && ((x.p[0] == 0 && !(x.p[1] & 0x80)) ||
                                                                       (x.p[0] == 0xff && (x.p[1] & 0x80)))))
        return BAD;
    c->serial = (x509_span_t){ x.p, x.n };
    // The signature algorithm, again: the same bytes as outside.
    if (der_whole(&tbs, DER_SEQUENCE, &alg2_whole, &alg2) != 0 || !span_eq(alg_whole, alg2_whole)) return BAD;
    if (der_whole(&tbs, DER_SEQUENCE, &c->issuer, &x) != 0 || check_name(x) != 0) return BAD;
    if (der_expect(&tbs, DER_SEQUENCE, &v) != 0 || read_time(&v, &c->not_before) != 0 ||
        read_time(&v, &c->not_after) != 0 || v.n)
        return BAD;
    if (der_whole(&tbs, DER_SEQUENCE, &c->subject, &x) != 0 || check_name(x) != 0) return BAD;

    // SubjectPublicKeyInfo: keys of other kinds read, but are of no use.
    if (der_expect(&tbs, DER_SEQUENCE, &spki) != 0 || der_expect(&spki, DER_SEQUENCE, &key_alg) != 0 ||
        der_expect(&key_alg, DER_OID, &oid) != 0 || der_expect(&spki, DER_BIT_STRING, &x) != 0 || spki.n || !x.n ||
        x.p[0])
        return BAD;
    der_t key = der_of(x.p + 1, x.n - 1);
    if (oid_is(oid, OID_RSA, sizeof OID_RSA)) {
        der_t rsa, null;
        const uint8_t *n, *e;
        uint32_t n_len, e_len;
        if (der_expect(&key_alg, DER_NULL, &null) != 0 || null.n || key_alg.n) return BAD;
        if (der_expect(&key, DER_SEQUENCE, &rsa) != 0 || key.n || der_uint(&rsa, &n, &n_len) != 0 ||
            der_uint(&rsa, &e, &e_len) != 0 || rsa.n || !n[0])
            return BAD;
        c->key_type = X509_KEY_RSA;
        c->key_a = (x509_span_t){ n, n_len };
        c->key_b = (x509_span_t){ e, e_len };
        c->key_bits = 8 * n_len;
        for (uint8_t top = n[0]; !(top & 0x80); top = (uint8_t)(top << 1)) c->key_bits--;
    } else if (oid_is(oid, OID_EC, sizeof OID_EC)) {
        der_t curve;
        if (der_peek(&key_alg, DER_OID) && der_expect(&key_alg, DER_OID, &curve) == 0 && !key_alg.n) {
            c->curve = oid_is(curve, OID_P256, sizeof OID_P256) ? CURVE_P256 :
                       oid_is(curve, OID_P384, sizeof OID_P384) ? CURVE_P384 : 0;
            if (c->curve) {
                c->key_type = X509_KEY_EC;
                c->key_a = (x509_span_t){ key.p, key.n };
                c->key_bits = 8 * curve_size(c->curve);
            }
        }
    }

    // Unique identifiers ([1], [2]): allowed, unused. Then extensions.
    uint8_t tag;
    if (der_peek(&tbs, DER_IMPLICIT(1)) && der_next(&tbs, &tag, &x) != 0) return BAD;
    if (der_peek(&tbs, DER_IMPLICIT(2)) && der_next(&tbs, &tag, &x) != 0) return BAD;
    if (der_peek(&tbs, DER_CONTEXT(3))) {
        der_t exts;
        if (der_expect(&tbs, DER_CONTEXT(3), &v) != 0 || der_expect(&v, DER_SEQUENCE, &exts) != 0 || v.n || !exts.n)
            return BAD;
        const uint8_t* first = exts.p;
        while (exts.n) {
            der_t ext, value, crit;
            int critical = 0;
            if (der_expect(&exts, DER_SEQUENCE, &ext) != 0 || der_expect(&ext, DER_OID, &oid) != 0) return BAD;
            if (der_peek(&ext, DER_BOOLEAN)) {
                if (der_expect(&ext, DER_BOOLEAN, &crit) != 0 || crit.n != 1 || crit.p[0] != 0xff) return BAD;
                critical = 1;
            }
            if (der_expect(&ext, DER_OCTET_STRING, &value) != 0 || ext.n) return BAD;
            // The same extension twice is refused (4.2): look back for this OID.
            der_t back = der_of(first, (uint32_t)(oid.p - first)), e2, o2;
            while (back.n > 0 && der_expect(&back, DER_SEQUENCE, &e2) == 0 && der_expect(&e2, DER_OID, &o2) == 0)
                if (o2.p < oid.p && o2.n == oid.n && memcmp(o2.p, oid.p, oid.n) == 0) return BAD;
            int r = read_extension(c, oid, critical, value);
            if (r) return r;
        }
    }
    return tbs.n || tbs.bad ? BAD : 0;
}

// ---- Signatures ----

// Keys strong enough to trust: RSA of 2048 bits or more, a whole number
// of bytes; P-256 and P-384.
static int key_ok(const x509_t* c) {
    if (c->key_type == X509_KEY_RSA) return c->key_bits >= 2048 && c->key_bits % 8 == 0 && c->key_bits <= 4096;
    return c->key_type == X509_KEY_EC;
}

int x509_check_signature(const x509_t* issuer, const x509_t* child) {
    if (!key_ok(issuer) || !child->sig_kind) return UNSUPPORTED;
    int r;
    if (issuer->key_type == X509_KEY_RSA && child->sig_kind == SIGK_RSA)
        r = rsa_pkcs1_verify(issuer->key_a.p, issuer->key_a.n, issuer->key_b.p, issuer->key_b.n, child->sig_hash,
                             child->tbs.p, child->tbs.n, child->sig.p, child->sig.n);
    else if (issuer->key_type == X509_KEY_EC && child->sig_kind == SIGK_ECDSA)
        r = ecdsa_verify(issuer->curve, issuer->key_a.p, issuer->key_a.n, child->sig_hash, child->tbs.p, child->tbs.n,
                         child->sig.p, child->sig.n);
    else return TLS_E_CERT_SIGNATURE;
    return r == SIG_OK ? 0 : r == SIG_UNSUPPORTED ? UNSUPPORTED : TLS_E_CERT_SIGNATURE;
}

// ---- Names ----

static uint8_t lower(uint8_t ch) { return ch >= 'A' && ch <= 'Z' ? (uint8_t)(ch + 32) : ch; }

static int same_ci(const uint8_t* a, const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

// Whether name is base or below it (a.b.c is below b.c and c).
static int dns_within(const uint8_t* name, uint32_t n, const uint8_t* base, uint32_t bn) {
    if (!bn) return 1;
    if (n == bn) return same_ci(name, base, n);
    return n > bn && name[n - bn - 1] == '.' && same_ci(name + n - bn, base, bn);
}

// Whether a certificate's DNS name (perhaps "*.x") covers the name asked for.
static int dns_matches(const uint8_t* pat, uint32_t pn, const uint8_t* name, uint32_t n) {
    if (pn >= 2 && pat[0] == '*') {
        // The wildcard stands for exactly one whole label.
        uint32_t i = 0;
        while (i < n && name[i] != '.') i++;
        return i > 0 && i < n && n - i == pn - 1 && same_ci(name + i, pat + 1, pn - 1);
    }
    return pn == n && same_ci(pat, name, n);
}

static int has_dot(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i] == '.') return 1;
    return 0;
}

// A DNS name constraint against a certificate's name: in the subtree
// (1), or, for excluded subtrees, a wildcard that could reach into it.
static int dns_constraint(const uint8_t* name, uint32_t n, const uint8_t* base, uint32_t bn, int excluded) {
    int wild = n >= 2 && name[0] == '*';
    const uint8_t* rest = wild ? name + 2 : name;
    uint32_t rn = wild ? n - 2 : n;
    if (dns_within(rest, rn, base, bn)) return 1;
    if (!excluded || !wild || bn <= rn + 1) return 0;
    // *.b.c and an excluded x.b.c: the wildcard could be x.
    uint32_t label = bn - rn - 1;
    return base[label] == '.' && same_ci(base + label + 1, rest, rn) && !has_dot(base, label);
}

// Whether the RDNs of constraint (a Name's contents) begin those of name.
static int dn_within(der_t name, der_t constraint) {
    while (constraint.n) {
        der_t a, b;
        x509_span_t wa, wb;
        if (der_whole(&constraint, DER_SET, &wa, &a) != 0 || der_whole(&name, DER_SET, &wb, &b) != 0) return 0;
        if (!span_eq(wa, wb)) return 0;
    }
    return 1;
}

// One name of a certificate (tag: its GeneralName tag, directory names
// as [4]) against one CA's constraints. 0, or why not.
static int name_vs_constraints(const x509_t* ca, uint8_t tag, der_t name, uint32_t* work) {
    for (int excl = 0; excl < 2; excl++) {
        der_t subtrees = der_of(excl ? ca->nc_excluded.p : ca->nc_permitted.p, excl ? ca->nc_excluded.n : ca->nc_permitted.n);
        int any = 0, hit = 0;
        while (subtrees.n) {
            der_t st, base;
            uint8_t btag;
            der_expect(&subtrees, DER_SEQUENCE, &st);
            der_next(&st, &btag, &base);
            if (subtrees.bad || st.bad) return BAD;
            if (btag != tag) continue;
            if (++*work > 4096) return BAD;  // a certificate built to take forever
            any = 1;
            if (tag == DER_IMPLICIT(2)) hit |= dns_constraint(name.p, name.n, base.p, base.n, excl);
            else if (tag == DER_IMPLICIT(7)) {
                if (base.n != 2 * name.n) continue;  // the other family
                int in = 1;
                for (uint32_t i = 0; i < name.n; i++)
                    if ((name.p[i] & base.p[name.n + i]) != base.p[i]) in = 0;
                hit |= in;
            } else if (tag == DER_CONTEXT(4)) {
                der_t cn;
                if (der_expect(&base, DER_SEQUENCE, &cn) != 0) return BAD;
                hit |= dn_within(name, cn);
            } else return UNSUPPORTED;  // e-mail, URI and other names: not checked here, so not passed
        }
        if (excl && hit) return BAD;
        if (!excl && any && !hit) return BAD;
    }
    return 0;
}

// All of a certificate's names against one CA's constraints.
static int names_vs_constraints(const x509_t* ca, const x509_t* c, uint32_t* work) {
    der_t subject, s;
    if (der_expect(&(der_t){ c->subject.p, c->subject.n, 0 }, DER_SEQUENCE, &subject) != 0) return BAD;
    if (subject.n) {
        int r = name_vs_constraints(ca, DER_CONTEXT(4), subject, work);
        if (r) return r;
    }
    s = der_of(c->san.p, c->san.n);
    while (s.n) {
        uint8_t tag;
        der_t v;
        if (der_next(&s, &tag, &v) != 0) return BAD;
        if (tag == DER_CONTEXT(4) && der_expect(&v, DER_SEQUENCE, &v) != 0) return BAD;
        int r = name_vs_constraints(ca, tag, v, work);
        if (r) return r;
    }
    return 0;
}

// The leaf's name: a DNS name, or an IP address, among its subjectAltNames.
static int name_matches(const x509_t* leaf, const char* dns, const uint8_t* ip, uint32_t ip_len) {
    uint32_t dn = dns ? (uint32_t)strlen(dns) : 0;
    if (dns && !dns_ok((const uint8_t*)dns, dn, 0)) return 0;
    der_t s = der_of(leaf->san.p, leaf->san.n);
    while (s.n) {
        uint8_t tag;
        der_t v;
        if (der_next(&s, &tag, &v) != 0) return 0;
        if (dns && tag == DER_IMPLICIT(2) && dns_matches(v.p, v.n, (const uint8_t*)dns, dn)) return 1;
        if (!dns && tag == DER_IMPLICIT(7) && v.n == ip_len && memcmp(v.p, ip, ip_len) == 0) return 1;
    }
    return 0;
}

// ---- Paths ----

#define MAX_SENT   16  // certificates read from the server
#define MAX_PATH   10  // certificates in a path, the leaf and root included
#define MAX_CHECKS 64  // signatures checked looking for one

typedef struct {
    const x509_policy_t* pol;
    const char* dns;
    const uint8_t* ip;
    uint32_t ip_len;
    x509_t* sent;  // [0] the leaf
    int* usable;
    uint32_t n_sent;
    const x509_t* path[MAX_PATH];
    int on_path[MAX_SENT];
    uint32_t len;
    int checks, err;
} walk_t;

static int self_issued(const x509_t* c) { return span_eq(c->issuer, c->subject); }

static int empty_name(x509_span_t n) { return n.n == 2; }

// Everything about a whole path (path[0] the leaf, the last the trusted
// root) but the signatures, checked as it was found.
static int check_path(walk_t* w) {
    const x509_policy_t* pol = w->pol;
    uint32_t len = w->len, depth = 0;
    for (uint32_t i = 0; i < len; i++) {
        const x509_t* c = w->path[i];
        int anchor = i == len - 1;
        if (pol->now < c->not_before || pol->now > c->not_after) return TLS_E_CERT_EXPIRED;
        if (!anchor) {
            // A positive serial number of at most 20 bytes.
            if ((c->serial.p[0] & 0x80) || c->serial.n > 21 || (c->serial.n == 21 && c->serial.p[0]) ||
                (c->serial.n == 1 && !c->serial.p[0]))
                return BAD;
            if (empty_name(c->issuer)) return BAD;
        }
        // subjectAltName: critical exactly when the subject is empty (4.2.1.6).
        if (c->has_san && c->san_critical != empty_name(c->subject)) return BAD;
        if (i == 0) continue;
        // A CA: says so in a critical basicConstraints, may sign
        // certificates, has a name, and is not too far up.
        if (!c->has_bc || !c->bc_critical || !c->is_ca || empty_name(c->subject)) return BAD;
        if (c->has_ku && !(c->ku & X509_KU_KEY_CERT_SIGN)) return BAD;
        if (!anchor && c->has_eku && !c->eku_server_auth && !c->eku_any) return BAD;
        if (!self_issued(c) && !anchor) depth++;
        uint32_t below = 0;
        for (uint32_t j = 1; j < i; j++) below += !self_issued(w->path[j]);
        if (c->path_len >= 0 && below > (uint32_t)c->path_len) return BAD;
    }
    if (depth > pol->max_depth) return TLS_E_CERT_UNKNOWN_CA;

    // The leaf: not a CA, for TLS servers, for this name.
    const x509_t* leaf = w->path[0];
    if ((leaf->has_bc && leaf->is_ca) || (leaf->has_ku && (leaf->ku & X509_KU_KEY_CERT_SIGN)) || leaf->has_nc)
        return BAD;
    if (leaf->has_ku && (leaf->ku & pol->leaf_ku) != pol->leaf_ku) return BAD;
    if (leaf->has_eku && (!leaf->eku_server_auth || leaf->eku_any)) return BAD;  // any use is no use (CABF 7.1.2.7.10)
    if (!key_ok(leaf)) return UNSUPPORTED;

    // Name constraints, of each CA over every certificate below it but
    // the self-issued CAs (4.2.1.10); the leaf is always checked.
    uint32_t work = 0;
    for (uint32_t j = 1; j < len; j++) {
        if (!w->path[j]->has_nc) continue;
        for (uint32_t i = 0; i < j; i++) {
            if (i > 0 && self_issued(w->path[i])) continue;
            int r = names_vs_constraints(w->path[j], w->path[i], &work);
            if (r) return r;
        }
    }
    if (!leaf->has_san || !name_matches(leaf, w->dns, w->ip, w->ip_len)) return TLS_E_CERT_NAME;
    return 0;
}

// Whether issuer signed child: names first, signatures (the expensive
// part) only within the budget.
static int signed_by(walk_t* w, const x509_t* child, const x509_t* issuer) {
    if (!span_eq(child->issuer, issuer->subject)) return 0;
    if (++w->checks > MAX_CHECKS) return 0;
    return x509_check_signature(issuer, child) == 0;
}

static void note(walk_t* w, int err) {
    if (w->err == TLS_E_CERT_UNKNOWN_CA) w->err = err;
}

// Depth first: from the last certificate on the path, to a root that
// signed it, or else to any sent certificate not yet on the path.
static int walk(walk_t* w) {
    const x509_t* cur = w->path[w->len - 1];
    if (w->len >= MAX_PATH) return -1;
    const x509_roots_t* roots = w->pol->roots;
    for (uint32_t i = 0; roots && i < roots->n; i++) {
        if (!signed_by(w, cur, &roots->cert[i])) continue;
        w->path[w->len++] = &roots->cert[i];
        int r = check_path(w);
        w->len--;
        if (!r) return 0;
        note(w, r);
    }
    for (uint32_t i = 1; i < w->n_sent; i++) {
        if (!w->usable[i] || w->on_path[i] || !signed_by(w, cur, &w->sent[i])) continue;
        w->on_path[i] = 1;
        w->path[w->len++] = &w->sent[i];
        int r = walk(w);
        w->len--;
        w->on_path[i] = 0;
        if (!r) return 0;
    }
    return -1;
}

int x509_verify_chain(const x509_policy_t* pol, const uint8_t* const* cert, const uint32_t* cert_len, uint32_t n,
                      const char* dns, const uint8_t* ip, uint32_t ip_len) {
    if (!n) return BAD;
    if (n > MAX_SENT) n = MAX_SENT;
    x509_t* sent = malloc(n * sizeof *sent);
    int* usable = malloc(n * sizeof *usable);
    if (!sent || !usable) {
        free(sent);
        free(usable);
        return TLS_E_NOMEM;
    }
    int r = x509_parse(&sent[0], cert[0], cert_len[0]);
    if (!r) {
        for (uint32_t i = 1; i < n; i++) usable[i] = x509_parse(&sent[i], cert[i], cert_len[i]) == 0;
        walk_t w;
        memset(&w, 0, sizeof w);
        w.pol = pol;
        w.dns = dns;
        w.ip = ip;
        w.ip_len = ip_len;
        w.sent = sent;
        w.usable = usable;
        w.n_sent = n;
        w.path[0] = &sent[0];
        w.len = 1;
        w.err = TLS_E_CERT_UNKNOWN_CA;
        r = walk(&w) == 0 ? 0 : w.err;
    }
    free(sent);
    free(usable);
    return r;
}

// ---- Roots ----

int x509_roots_add(x509_roots_t* r, const uint8_t* der, uint32_t len) {
    if (r->n == r->cap) {
        uint32_t cap = r->cap ? 2 * r->cap : 64;
        x509_t* c = realloc(r->cert, cap * sizeof *c);
        if (!c) return TLS_E_NOMEM;
        r->cert = c;
        uint8_t** d = realloc(r->der, cap * sizeof *d);
        if (!d) return TLS_E_NOMEM;
        r->der = d;
        r->cap = cap;
    }
    uint8_t* copy = malloc(len ? len : 1);
    if (!copy) return TLS_E_NOMEM;
    memcpy(copy, der, len);
    int e = x509_parse(&r->cert[r->n], copy, len);
    if (e) {
        free(copy);
        return e;
    }
    r->der[r->n++] = copy;
    return 0;
}

static int b64(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

static const char* find(const char* p, const char* end, const char* what) {
    uint32_t n = (uint32_t)strlen(what);
    for (; p + n <= end; p++)
        if (memcmp(p, what, n) == 0) return p;
    return 0;
}

int x509_roots_add_pem(x509_roots_t* r, const char* pem, uint32_t len) {
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end_mark[] = "-----END CERTIFICATE-----";
    const char *p = pem, *end = pem + len;
    int added = 0;
    while ((p = find(p, end, begin)) != 0) {
        p += sizeof begin - 1;
        const char* stop = find(p, end, end_mark);
        if (!stop) break;
        uint8_t* der = malloc((uint32_t)(stop - p) * 3 / 4 + 3);
        if (!der) return -1;
        uint32_t n = 0, acc = 0, bits = 0;
        int ok = 1, pad = 0;
        for (const char* q = p; q < stop; q++) {
            uint8_t ch = (uint8_t)*q;
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            if (ch == '=') {
                pad++;
                continue;
            }
            int v = b64(ch);
            if (v < 0 || pad) {
                ok = 0;
                break;
            }
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der[n++] = (uint8_t)(acc >> bits);
            }
        }
        if (ok) {
            int e = x509_roots_add(r, der, n);
            if (e == TLS_E_NOMEM) {
                free(der);
                return -1;
            }
            added += e == 0;
        }
        free(der);
        p = stop + sizeof end_mark - 1;
    }
    return added;
}

void x509_roots_free(x509_roots_t* r) {
    for (uint32_t i = 0; i < r->n; i++) free(r->der[i]);
    free(r->der);
    free(r->cert);
    memset(r, 0, sizeof *r);
}

// ---- TLS ----

// "a.b.c.d" as four bytes; 0 if it is not one.
static int ipv4(const char* s, uint8_t* out) {
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0, digits_seen = 0;
        while (*s >= '0' && *s <= '9' && digits_seen < 4) {
            v = v * 10 + (uint32_t)(*s++ - '0');
            digits_seen++;
        }
        if (!digits_seen || v > 255 || (digits_seen > 1 && s[-(int)digits_seen] == '0')) return 0;
        out[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return 0;
    }
    return *s == 0;
}

int x509_tls_verify(void* ctx, const tls_peer_t* peer) {
    const x509_policy_t* pol = ctx;
    uint8_t ip[4];
    int is_ip = ipv4(peer->name, ip);
    int r = x509_verify_chain(pol, peer->cert, peer->cert_len, peer->n_certs, is_ip ? 0 : peer->name, ip, is_ip ? 4 : 0);
    if (r) return r;
    x509_t leaf;
    if (x509_parse(&leaf, peer->cert[0], peer->cert_len[0]) != 0) return BAD;
    int s;
    switch (peer->scheme) {
    case 0x0403:
    case 0x0503:
        if (leaf.key_type != X509_KEY_EC || leaf.curve != (peer->scheme == 0x0403 ? CURVE_P256 : CURVE_P384))
            return TLS_E_CERT_SIGNATURE;
        s = ecdsa_verify(leaf.curve, leaf.key_a.p, leaf.key_a.n, peer->scheme == 0x0403 ? HASH_SHA256 : HASH_SHA384,
                         peer->signed_content, peer->signed_len, peer->signature, peer->signature_len);
        break;
    case 0x0804:
    case 0x0805:
    case 0x0806: {
        int hash = peer->scheme - 0x0804 + HASH_SHA256;
        if (leaf.key_type != X509_KEY_RSA) return TLS_E_CERT_SIGNATURE;
        s = rsa_pss_verify(leaf.key_a.p, leaf.key_a.n, leaf.key_b.p, leaf.key_b.n, hash, hash_size(hash),
                           peer->signed_content, peer->signed_len, peer->signature, peer->signature_len);
        break;
    }
    default:
        return TLS_E_CERT_SIGNATURE;
    }
    return s == SIG_OK ? 0 : TLS_E_CERT_SIGNATURE;
}

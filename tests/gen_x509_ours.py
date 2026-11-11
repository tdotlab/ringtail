"""Our own certificate cases for test_x509, beside x509-limbo's: each
made byte by byte (a small DER writer) and then properly signed, so that
only the one thing a case is about is wrong. Limbo is about paths; these
are mostly about reading: times, names, extensions, the shape of the
certificate. Each says which error it must give, not just that it fails.

Keys are fixed (EC scalars, RSA primes from a seeded generator) and the
ECDSA signatures deterministic (RFC 6979), so the output does not change
from one run to the next."""
import random
import zlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa

BAD, UNSUPPORTED, EXPIRED, UNKNOWN_CA, NAME = -1020, -1021, -1022, -1023, -1024
WHEN = "20260601120000"  # every case is checked at this moment


# ---- DER ----

def tlv(tag, *parts):
    body = b"".join(parts)
    n = len(body)
    if n < 0x80:
        head = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        head = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + head + body


def seq(*p): return tlv(0x30, *p)
def set_(*p): return tlv(0x31, *p)
def octets(b): return tlv(0x04, b)
def bits(b, unused=0): return tlv(0x03, bytes([unused]) + b)
def utc(s): return tlv(0x17, s.encode())
def gen(s): return tlv(0x18, s.encode())
def explicit(n, *p): return tlv(0xA0 | n, *p)
def implicit(n, b): return tlv(0x80 | n, b)


NULL = tlv(0x05)
TRUE = tlv(0x01, b"\xff")


def oid(dotted):
    a = [int(x) for x in dotted.split(".")]
    out = bytearray()
    for v in [40 * a[0] + a[1]] + a[2:]:
        chunk = [v & 0x7F]
        v >>= 7
        while v:
            chunk.append(0x80 | (v & 0x7F))
            v >>= 7
        out += bytes(reversed(chunk))
    return tlv(0x06, bytes(out))


def integer(v):
    if isinstance(v, bytes):
        return tlv(0x02, v)
    n = 1
    while True:
        try:
            return tlv(0x02, v.to_bytes(n, "big", signed=True))
        except OverflowError:
            n += 1


def name(*rdns):
    """name(("CN", "x"), ("O", "y")): one attribute an RDN."""
    types = {"CN": "2.5.4.3", "O": "2.5.4.10", "C": "2.5.4.6", "OU": "2.5.4.11"}
    return seq(*[set_(seq(oid(types[t]), tlv(0x0C, v.encode()))) for t, v in rdns])


def ext(dotted, value, critical=False):
    return seq(oid(dotted), *([TRUE] if critical else []), octets(value))


def bc(ca=True, pathlen=None, critical=True):
    return ext("2.5.29.19", seq(*([TRUE] if ca else []), *([integer(pathlen)] if pathlen is not None else [])), critical)


KU_BITS = {"digitalSignature": 0, "keyEncipherment": 2, "keyCertSign": 5, "cRLSign": 6}


def ku(*names, critical=True):
    v = 0
    for nm in names:
        v |= 1 << (15 - KU_BITS[nm])
    b = v.to_bytes(2, "big")
    if b[1] == 0:
        b = b[:1]
    unused = 0
    last = b[-1]
    while unused < 7 and not (last >> unused) & 1:
        unused += 1
    return ext("2.5.29.15", bits(b, unused), critical)


SERVER_AUTH, CLIENT_AUTH, ANY_EKU = "1.3.6.1.5.5.7.3.1", "1.3.6.1.5.5.7.3.2", "2.5.29.37.0"


def eku(*oids):
    return ext("2.5.29.37", seq(*[oid(o) for o in oids]))


def dns(s): return implicit(2, s.encode())
def ip(*b): return implicit(7, bytes(b))
def dirname(n): return tlv(0xA4, n)
def email(s): return implicit(1, s.encode())


def san(*names, critical=False):
    return ext("2.5.29.17", seq(*names), critical)


def nc(permitted=(), excluded=(), critical=True):
    parts = []
    if permitted:
        parts.append(explicit(0, *[seq(g) for g in permitted]))
    if excluded:
        parts.append(explicit(1, *[seq(g) for g in excluded]))
    return ext("2.5.29.30", seq(*parts), critical)


# ---- Keys ----

def _prime(rng, nbits):
    while True:
        c = rng.getrandbits(nbits) | 1 << (nbits - 1) | 1
        if all(pow(a, c - 1, c) == 1 for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37)):
            return c


def rsa_key(nbits, seed):
    rng = random.Random(seed)
    while True:
        p, q = _prime(rng, (nbits + 1) // 2), _prime(rng, nbits // 2)
        n = p * q
        if p != q and n.bit_length() == nbits and (p - 1) % 65537 and (q - 1) % 65537:
            break
    d = pow(65537, -1, (p - 1) * (q - 1))
    pub = rsa.RSAPublicNumbers(65537, n)
    return rsa.RSAPrivateNumbers(p, q, d, d % (p - 1), d % (q - 1), pow(q, -1, p), pub).private_key()


def ec_key(scalar, curve=ec.SECP256R1):
    return ec.derive_private_key(scalar, curve())


def spki(key):
    return key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


ALGS = {
    ("ec", "sha256"): seq(oid("1.2.840.10045.4.3.2")),
    ("ec", "sha384"): seq(oid("1.2.840.10045.4.3.3")),
    ("ec", "sha512"): seq(oid("1.2.840.10045.4.3.4")),
    ("rsa", "sha256"): seq(oid("1.2.840.113549.1.1.11"), NULL),
    ("rsa", "sha384"): seq(oid("1.2.840.113549.1.1.12"), NULL),
    ("rsa", "sha512"): seq(oid("1.2.840.113549.1.1.13"), NULL),
    ("rsa", "sha1"): seq(oid("1.2.840.113549.1.1.5"), NULL),
}
HASHES = {"sha1": hashes.SHA1, "sha256": hashes.SHA256, "sha384": hashes.SHA384, "sha512": hashes.SHA512}


def kind(key):
    return "ec" if isinstance(key, ec.EllipticCurvePrivateKey) else "rsa"


def sign(key, data, h):
    if kind(key) == "ec":
        return key.sign(data, ec.ECDSA(HASHES[h](), deterministic_signing=True))
    return key.sign(data, padding.PKCS1v15(), HASHES[h]())


class Cert:
    """A certificate to be: fields as DER, any of them replaceable."""

    def __init__(self, subject, key, issuer=None, exts=(), h="sha256", serial=None):
        self.key = key
        self.issuer = issuer or self  # a Cert (self: self-signed)
        self.h = h
        self.f = {
            "version": explicit(0, integer(2)),
            "serial": integer(serial if serial is not None else 0x1000 + (zlib.crc32(subject) & 0xFFFF)),
            "alg": None,  # from the issuer's key
            "issuer": None,
            "validity": seq(utc("250101000000Z"), utc("271231235959Z")),
            "subject": subject,
            "spki": spki(key),
            "unique": b"",
            "exts": list(exts),
        }
        self.outer_alg = None  # None: the same as inside
        self.sig_bits = None  # None: the real signature
        self.after = b""  # bytes after the Certificate
        self.raw_exts = None  # the [3] field whole, in place of exts

    def der(self):
        f = dict(self.f)
        ik = self.issuer.key
        alg = f["alg"] if f["alg"] is not None else ALGS[(kind(ik), self.h)]
        exts = self.raw_exts if self.raw_exts is not None else (explicit(3, seq(*f["exts"])) if f["exts"] else b"")
        tbs = seq(f["version"], f["serial"], alg, f["issuer"] if f["issuer"] is not None else self.issuer.f["subject"],
                  f["validity"], f["subject"], f["spki"], f["unique"], exts)
        sig = bits(sign(ik, tbs, self.h)) if self.sig_bits is None else self.sig_bits
        if self.sig_bits is None and self.flip:
            s = bytearray(sig)
            s[-5] ^= 1
            sig = bytes(s)
        return seq(tbs, self.outer_alg if self.outer_alg is not None else alg, sig) + self.after

    flip = False

    def but(self, **kw):
        """A copy with these fields replaced (by name, or the attributes)."""
        c = Cert.__new__(Cert)
        c.__dict__ = dict(self.__dict__)
        c.f = dict(self.f)
        c.f["exts"] = list(self.f["exts"])
        for k, v in kw.items():
            if k in c.f:
                c.f[k] = v
            else:
                setattr(c, k, v)
        return c


def ca_exts(pathlen=None):
    return [bc(True, pathlen), ku("keyCertSign", "cRLSign")]


def leaf_exts(*names):
    return [san(*(names or (dns("example.com"), dns("*.wild.example"), ip(192, 0, 2, 1)))),
            ku("digitalSignature"), eku(SERVER_AUTH)]


# ---- The cases ----

def ours():
    root_key = ec_key(0x5EED0001)
    root = Cert(name(("CN", "Test Root"), ("O", "RingTail")), root_key, exts=ca_exts())
    rsa_ca_key = rsa_key(2048, 1)
    inter = Cert(name(("CN", "Test RSA Intermediate")), rsa_ca_key, root, exts=ca_exts(0))
    leaf_key = ec_key(0x5EED0002)
    leaf = Cert(name(("CN", "example.com")), leaf_key, root, exts=leaf_exts())

    cases = []

    def case(cid, leaf_cert, ok, err=0, chain=(), roots=(root,), host="example.com", addr="", when=WHEN,
             max_depth=6, leaf_ku=1):
        if ok:
            assert err == 0
        cases.append(dict(id="ours::" + cid, ok=ok, err=err, leaf=leaf_cert.der(), inter=[c.der() for c in chain],
                          roots=[r.der() for r in roots], when=when, dns=host, ip=addr, max_depth=max_depth,
                          leaf_ku=leaf_ku))

    def good(cid, c, **kw): case(cid, c, True, **kw)
    def bad(cid, c, err, **kw): case(cid, c, False, err, **kw)

    def with_exts(*e): return leaf.but(exts=list(e))
    def plus(*e): return leaf.but(exts=leaf.f["exts"] + list(e))
    def valid(nb, na): return leaf.but(validity=seq(nb, na))

    # The ordinary ones.
    good("basic", leaf)
    good("name-case", leaf, host="EXAMPLE.Com")
    good("wildcard", leaf, host="a.wild.example")
    good("ip", leaf, host="", addr="c0000201")
    good("rsa-intermediate", Cert(leaf.f["subject"], leaf_key, inter, exts=leaf_exts()), chain=(inter,))
    for h in ("sha256", "sha384", "sha512"):
        good("rsa-signed-" + h, Cert(leaf.f["subject"], leaf_key, inter, exts=leaf_exts(), h=h), chain=(inter,))
        good("ecdsa-signed-" + h, leaf.but(h=h))
    good("rsa-leaf", Cert(leaf.f["subject"], rsa_key(2048, 2), root, exts=leaf_exts()))
    good("rsa-3072-leaf", Cert(leaf.f["subject"], rsa_key(3072, 3), root, exts=leaf_exts()))
    good("p384-leaf", Cert(leaf.f["subject"], ec_key(0x5EED0003, ec.SECP384R1), root, exts=leaf_exts()))
    good("p384-root", Cert(leaf.f["subject"], leaf_key, (r384 := Cert(name(("CN", "Root 384")),
                           ec_key(0x5EED0004, ec.SECP384R1), exts=ca_exts())), exts=leaf_exts(), h="sha384"),
         roots=(r384,))
    good("rsa-root", Cert(leaf.f["subject"], leaf_key, (rr := Cert(name(("CN", "RSA Root")), rsa_key(4096, 4),
                          exts=ca_exts())), exts=leaf_exts()), roots=(rr,))
    good("no-ku-no-eku", with_exts(san(dns("example.com"))))
    good("eku-server-and-client", with_exts(san(dns("example.com")), eku(SERVER_AUTH, CLIENT_AUTH)))
    good("bc-not-ca", plus(bc(False)))
    good("bc-not-ca-pathlen-ignored", plus(bc(False, 3)))
    good("unknown-noncritical", plus(ext("1.2.3.4.5", NULL)))
    good("policies-critical", plus(ext("2.5.29.32", seq(seq(oid("2.23.140.1.2.1"))), True)))
    good("aia", plus(ext("1.3.6.1.5.5.7.1.1", seq(seq(oid("1.3.6.1.5.5.7.48.1"), implicit(6, b"http://o.example/"))))))
    good("ski-aki", plus(ext("2.5.29.14", octets(b"\x01" * 20)), ext("2.5.29.35", seq(implicit(0, b"\x02" * 20)))))
    good("dirname-in-san", with_exts(san(dns("example.com"), dirname(name(("CN", "x"), ("O", "y"))))))
    good("email-in-san", with_exts(san(email("a@example.com"), dns("example.com"))))
    good("unique-ids", leaf.but(unique=implicit(1, b"\x00\x01") + implicit(2, b"\x00\x02")))
    good("serial-20-bytes", leaf.but(serial=integer(bytes([0x00, 0x80]) + bytes(19))))
    good("serial-1", leaf.but(serial=integer(1)))
    good("empty-subject-critical-san", leaf.but(subject=seq(), exts=[san(dns("example.com"), critical=True)]))
    good("dns-63-label", with_exts(san(dns("a" * 63 + ".example"))), host="a" * 63 + ".example")
    long_name = ".".join(["a" * 63] * 3 + ["b" * 61])  # 253
    good("dns-253", with_exts(san(dns(long_name))), host=long_name)
    good("dns-single-label", with_exts(san(dns("localhost"))), host="localhost")
    good("dns-digits-hyphens", with_exts(san(dns("1-2.x-y.example"))), host="1-2.x-y.example")
    good("ipv6-in-san", with_exts(san(ip(*([0x20, 0x01, 0x0d, 0xb8] + [0] * 11 + [1])), dns("example.com"))))
    good("extra-sent-certificates", leaf, chain=(inter, root, Cert(name(("CN", "Other")), ec_key(7), exts=ca_exts())))

    # Times.
    good("time-not-before-now", valid(utc("260601120000Z"), utc("271231235959Z")))
    good("time-not-after-now", valid(utc("250101000000Z"), utc("260601120000Z")))
    bad("time-not-yet", valid(utc("260601120001Z"), utc("271231235959Z")), EXPIRED)
    bad("time-over", valid(utc("250101000000Z"), utc("260601115959Z")), EXPIRED)
    good("time-utc-49-is-2049", valid(utc("250101000000Z"), utc("491231235959Z")))
    bad("time-utc-50-is-1950", valid(utc("250101000000Z"), utc("500101000000Z")), EXPIRED)
    good("time-generalized", valid(gen("20250101000000Z"), gen("20500101000000Z")))
    good("time-feb-29-leap", valid(utc("240229000000Z"), utc("271231235959Z")))
    good("time-feb-29-2000", valid(gen("20000229000000Z"), utc("271231235959Z")))
    for cid, t in [("nondigit", utc("25a101000000Z")), ("month-0", utc("250001000000Z")),
                   ("month-13", utc("251301000000Z")), ("day-0", utc("250100000000Z")),
                   ("day-32", utc("250132000000Z")), ("apr-31", utc("250431000000Z")),
                   ("feb-29-2025", utc("250229000000Z")), ("feb-29-2100", gen("21000229000000Z")),
                   ("hour-24", utc("250101240000Z")), ("minute-60", utc("250101006000Z")),
                   ("second-60", utc("250101000060Z")), ("no-z", tlv(0x17, b"2501010000000")),
                   ("no-seconds", utc("2501010000Z")), ("fraction", gen("20250101000000.5Z")),
                   ("offset", tlv(0x17, b"250101000000+0100"[:13])), ("utc-15", utc("20250101000000Z")),
                   ("generalized-13", gen("250101000000Z")), ("octets", octets(b"250101000000Z")),
                   ("space", utc("2501 1000000Z")), ("minus", utc("25-101000000Z")),
                   ("colon", utc("250:01000000Z"))]:  # ':' is the digit after '9', so "0:" would be 10
        bad("time-" + cid, valid(t, utc("271231235959Z")), BAD)
    bad("time-third", leaf.but(validity=seq(utc("250101000000Z"), utc("271231235959Z"), utc("271231235959Z"))), BAD)
    expired_root = root.but(validity=seq(utc("200101000000Z"), utc("250101000000Z")))
    bad("time-root-expired", Cert(leaf.f["subject"], leaf_key, expired_root, exts=leaf_exts()), EXPIRED,
        roots=(expired_root,))

    # Names asked for.
    bad("name-other", leaf, NAME, host="example.org")
    bad("name-wildcard-two-labels", leaf, NAME, host="a.b.wild.example")
    bad("name-wildcard-none", leaf, NAME, host="wild.example")
    bad("name-wildcard-empty", leaf, NAME, host=".wild.example")
    bad("name-suffix", leaf, NAME, host="xexample.com")
    bad("name-ip-other", leaf, NAME, host="", addr="c0000202")
    bad("name-ip-not-dns", with_exts(san(dns("192.0.2.1"))), NAME, host="", addr="c0000201")
    bad("name-dns-not-ip", leaf, NAME, host="192.0.2.1")  # asked for as a DNS name, it matches no DNS name
    bad("name-no-san", with_exts(ku("digitalSignature")), NAME)
    bad("name-trailing-dot", leaf, NAME, host="example.com.")
    bad("name-asked-wildcard", leaf, NAME, host="*.wild.example")

    # subjectAltName itself.
    for cid, g in [("empty-dns", dns("")), ("label-64", dns("a" * 64 + ".example")),
                   ("name-254", dns(long_name + "b")), ("leading-hyphen", dns("-a.example")),
                   ("trailing-hyphen", dns("a-.example")), ("underscore", dns("a_b.example")),
                   ("empty-label", dns("a..example")), ("trailing-dot", dns("example.com.")),
                   ("wildcard-tld", dns("*.com")), ("wildcard-partial", dns("a*.example.com")),
                   ("wildcard-middle", dns("a.*.example.com")), ("wildcard-alone", dns("*")),
                   ("ip-5", ip(1, 2, 3, 4, 5)), ("ip-3", ip(1, 2, 3)),
                   ("dirname-trailing", tlv(0xA4, name(("CN", "x")) + NULL)),
                   ("dirname-not-name", tlv(0xA4, NULL)), ("utf8", implicit(2, "exämple.com".encode()))]:
        bad("san-" + cid, with_exts(san(dns("example.com"), g)), BAD)
    bad("san-empty", with_exts(san()), BAD)
    bad("san-critical-with-subject", with_exts(san(dns("example.com"), critical=True)), BAD)
    bad("san-noncritical-empty-subject", leaf.but(subject=seq()), BAD)
    bad("san-twice", plus(san(dns("example.com"))), BAD)
    bad("san-trailing", with_exts(ext("2.5.29.17", seq(dns("example.com")) + NULL)), BAD)

    # The certificate's shape.
    bad("v1", leaf.but(version=b"", exts=[]), UNSUPPORTED)
    bad("v2", leaf.but(version=explicit(0, integer(1))), UNSUPPORTED)
    bad("v4", leaf.but(version=explicit(0, integer(3))), UNSUPPORTED)
    bad("version-trailing", leaf.but(version=explicit(0, integer(2), NULL)), BAD)
    bad("version-not-integer", leaf.but(version=explicit(0, octets(b"\x02"))), BAD)
    bad("serial-zero", leaf.but(serial=integer(0)), BAD)
    bad("serial-negative", leaf.but(serial=integer(-5)), BAD)
    bad("serial-not-minimal", leaf.but(serial=integer(b"\x00\x01")), BAD)
    bad("serial-not-minimal-negative", leaf.but(serial=integer(b"\xff\x80")), BAD)
    bad("serial-empty", leaf.but(serial=integer(b"")), BAD)
    bad("serial-21-bytes", leaf.but(serial=integer(b"\x01" + bytes(20))), BAD)
    bad("alg-mismatch", leaf.but(outer_alg=ALGS[("ec", "sha384")]), BAD)
    bad("alg-rsa-no-null", Cert(leaf.f["subject"], leaf_key, inter, exts=leaf_exts()).but(
        alg=seq(oid("1.2.840.113549.1.1.11"))), UNKNOWN_CA, chain=(inter,))
    bad("alg-ecdsa-null", leaf.but(alg=seq(oid("1.2.840.10045.4.3.2"), NULL)), UNKNOWN_CA)
    bad("alg-sha1", Cert(leaf.f["subject"], leaf_key, inter, exts=leaf_exts(), h="sha1"), UNKNOWN_CA, chain=(inter,))
    bad("alg-unknown", leaf.but(alg=seq(oid("1.2.3.4"))), UNKNOWN_CA)
    bad("trailing-after", leaf.but(after=NULL), BAD)
    bad("signature-unused-bits", leaf.but(sig_bits=bits(sign(root_key, b"x", "sha256"), 1)), BAD)
    bad("signature-empty", leaf.but(sig_bits=tlv(0x03)), BAD)
    bad("signature-wrong", leaf.but(flip=True), UNKNOWN_CA)
    bad("dn-empty-rdn", leaf.but(subject=seq(set_())), BAD)
    bad("dn-atv-extra", leaf.but(subject=seq(set_(seq(oid("2.5.4.3"), tlv(0x0C, b"x"), NULL)))), BAD)
    bad("dn-atv-no-value", leaf.but(subject=seq(set_(seq(oid("2.5.4.3"))))), BAD)
    bad("dn-atv-not-oid", leaf.but(subject=seq(set_(seq(NULL, tlv(0x0C, b"x"))))), BAD)
    bad("dn-not-set", leaf.but(subject=seq(seq(seq(oid("2.5.4.3"), tlv(0x0C, b"x"))))), BAD)
    bad("issuer-empty", leaf.but(issuer=seq()), UNKNOWN_CA)
    bad("extensions-empty", leaf.but(raw_exts=explicit(3, seq())), BAD)
    bad("extensions-trailing", leaf.but(raw_exts=explicit(3, seq(*leaf.f["exts"]), NULL)), BAD)
    bad("extension-not-octets", leaf.but(raw_exts=explicit(3, seq(seq(oid("1.2.3"), NULL)))), BAD)
    bad("extension-trailing", plus(seq(oid("1.2.3"), octets(NULL), NULL)), BAD)
    bad("extension-false-written", plus(seq(oid("1.2.3"), tlv(0x01, b"\x00"), octets(NULL))), BAD)
    bad("extension-true-not-ff", plus(seq(oid("1.2.3"), tlv(0x01, b"\x01"), octets(NULL))), BAD)
    bad("tbs-trailing", leaf.but(unique=NULL), BAD)

    # Keys.
    rsa_no_null = seq(seq(oid("1.2.840.113549.1.1.1")), bits(seq(integer(rsa_key(2048, 2).public_key().public_numbers().n),
                                                                   integer(65537))))
    bad("key-rsa-no-null", leaf.but(spki=rsa_no_null), BAD)
    bad("key-rsa-1024", Cert(leaf.f["subject"], rsa_key(1024, 5), root, exts=leaf_exts()), UNSUPPORTED)
    bad("key-rsa-2040", Cert(leaf.f["subject"], rsa_key(2040, 6), root, exts=leaf_exts()), UNSUPPORTED)
    bad("key-p521", Cert(leaf.f["subject"], ec_key(0x5EED0005, ec.SECP521R1), root, exts=leaf_exts()), UNSUPPORTED)
    bad("key-unused-bits", leaf.but(spki=seq(seq(oid("1.2.840.10045.2.1"), oid("1.2.840.10045.3.1.7")),
                                             bits(spki(leaf_key)[-65:], 1))), BAD)
    bad("key-spki-trailing", leaf.but(spki=seq(seq(oid("1.2.840.10045.2.1"), oid("1.2.840.10045.3.1.7")),
                                               bits(spki(leaf_key)[-65:]), NULL)), BAD)

    # Extensions of the leaf.
    bad("unknown-critical", plus(ext("1.2.3.4.5", NULL, True)), UNSUPPORTED)
    bad("unknown-ce-critical", plus(ext("2.5.29.99", NULL, True)), UNSUPPORTED)
    # Not under id-ce, but ending as an id-ce extension does: not one of them.
    bad("unknown-like-ski-critical", plus(ext("1.2.3.14", octets(b"\x01"), True)), UNSUPPORTED)
    good("unknown-like-san", plus(ext("1.2.3.17", NULL)))
    bad("aki-critical", plus(ext("2.5.29.35", seq(implicit(0, b"\x02" * 20)), True)), BAD)
    bad("ski-critical", plus(ext("2.5.29.14", octets(b"\x01" * 20), True)), BAD)
    bad("aia-critical", plus(ext("1.3.6.1.5.5.7.1.1", seq(seq(oid("1.3.6.1.5.5.7.48.1"),
                                                            implicit(6, b"http://o.example/"))), True)), BAD)
    bad("aia-empty", plus(ext("1.3.6.1.5.5.7.1.1", seq())), BAD)
    bad("ku-no-bits", with_exts(san(dns("example.com")), ext("2.5.29.15", bits(b""))), BAD)
    bad("ku-zero", with_exts(san(dns("example.com")), ext("2.5.29.15", bits(b"\x00"))), BAD)
    bad("ku-unused-8", with_exts(san(dns("example.com")), ext("2.5.29.15", bits(b"\x80", 8))), BAD)
    bad("ku-4-bytes", with_exts(san(dns("example.com")), ext("2.5.29.15", bits(b"\x80\x00\x00", 0))), BAD)
    good("ku-decipher-only", with_exts(san(dns("example.com")), ext("2.5.29.15", bits(b"\x80\x80", 7))))
    bad("ku-no-digital-signature", with_exts(san(dns("example.com")), ku("keyEncipherment")), BAD)
    good("ku-not-asked", with_exts(san(dns("example.com")), ku("keyEncipherment")), leaf_ku=0)
    bad("ku-cert-sign", with_exts(san(dns("example.com")), ku("digitalSignature", "keyCertSign")), BAD)
    bad("eku-empty", with_exts(san(dns("example.com")), ext("2.5.29.37", seq())), BAD)
    bad("eku-client-only", with_exts(san(dns("example.com")), eku(CLIENT_AUTH)), BAD)
    bad("eku-any", with_exts(san(dns("example.com")), eku(ANY_EKU)), BAD)
    bad("eku-not-oid", with_exts(san(dns("example.com")), ext("2.5.29.37", seq(NULL))), BAD)
    bad("leaf-ca", plus(bc(True)), BAD)
    bad("leaf-nc", plus(nc([dns("example.com")])), BAD)
    bad("bc-false-written", plus(ext("2.5.29.19", seq(tlv(0x01, b"\x00")), True)), BAD)
    bad("bc-pathlen-negative", plus(ext("2.5.29.19", seq(TRUE, integer(-1)), True)), BAD)
    bad("bc-pathlen-256", plus(ext("2.5.29.19", seq(TRUE, integer(256)), True)), BAD)
    bad("bc-trailing", plus(ext("2.5.29.19", seq(TRUE, integer(1), NULL), True)), BAD)

    # The CAs above it.
    def via(ca_exts_, *, chain_extra=(), h="sha256", subject=None, key=None):
        ca = Cert(subject or name(("CN", "Intermediate")), key or ec_key(0x5EED0010), root, exts=ca_exts_)
        return Cert(leaf.f["subject"], leaf_key, ca, exts=leaf_exts(), h=h), (ca,) + tuple(chain_extra)

    lf, ch = via(ca_exts())
    good("intermediate", lf, chain=ch)
    good("intermediate-not-needed-order", lf, chain=(Cert(name(("CN", "Other")), ec_key(8), exts=ca_exts()),) + ch)
    bad("intermediate-not-sent", lf, UNKNOWN_CA)
    bad("intermediate-unknown-root", lf, UNKNOWN_CA, chain=ch, roots=(Cert(name(("CN", "Other")), ec_key(9),
                                                                           exts=ca_exts()),))
    bad("no-roots", leaf, UNKNOWN_CA, roots=())
    lf, ch = via([ku("keyCertSign")])
    bad("ca-no-bc", lf, BAD, chain=ch)
    lf, ch = via([bc(True, critical=False), ku("keyCertSign")])
    bad("ca-bc-not-critical", lf, BAD, chain=ch)
    lf, ch = via([bc(False), ku("keyCertSign")])
    bad("ca-bc-not-ca", lf, BAD, chain=ch)
    lf, ch = via([bc(True), ku("digitalSignature")])
    bad("ca-ku-no-cert-sign", lf, BAD, chain=ch)
    lf, ch = via([bc(True)])
    good("ca-no-ku", lf, chain=ch)
    lf, ch = via(ca_exts() + [eku(CLIENT_AUTH)])
    bad("ca-eku-client-only", lf, BAD, chain=ch)
    lf, ch = via(ca_exts() + [eku(SERVER_AUTH)])
    good("ca-eku-server", lf, chain=ch)
    lf, ch = via(ca_exts() + [eku(ANY_EKU)])
    good("ca-eku-any", lf, chain=ch)
    lf, ch = via(ca_exts() + [ext("1.2.3.4.5", NULL, True)])
    bad("ca-unknown-critical", lf, UNKNOWN_CA, chain=ch)  # unreadable, so not a way up
    lf, ch = via(ca_exts(), key=rsa_key(1024, 7))
    bad("ca-rsa-1024", lf, UNKNOWN_CA, chain=ch)
    rsa_root_1024 = Cert(name(("CN", "Weak Root")), rsa_key(1024, 8), exts=ca_exts())
    bad("root-rsa-1024", Cert(leaf.f["subject"], leaf_key, rsa_root_1024, exts=leaf_exts()), UNKNOWN_CA,
        roots=(rsa_root_1024,))
    lf, ch = via(ca_exts(), subject=seq())
    bad("ca-empty-subject", lf, BAD, chain=ch)

    # Path lengths and depth: a chain of EC intermediates under the root.
    def chain_of(n, pathlens=None, self_issued_at=None):
        cas, up = [], root
        for i in range(n):
            pl = pathlens[i] if pathlens else None
            subj = name(("CN", "CA %d" % i))
            if self_issued_at is not None and i == self_issued_at + 1:
                subj = cas[-1].f["subject"]  # the same name: a new key under it
            c = Cert(subj, ec_key(0x5EED0100 + i), up, exts=ca_exts(pl))
            cas.append(c)
            up = c
        return Cert(leaf.f["subject"], leaf_key, up, exts=leaf_exts()), tuple(reversed(cas))

    lf, ch = chain_of(6)
    good("depth-6", lf, chain=ch)
    bad("depth-6-max-5", lf, UNKNOWN_CA, chain=ch, max_depth=5)
    lf, ch = chain_of(7)
    bad("depth-7", lf, UNKNOWN_CA, chain=ch)
    good("depth-7-max-7", lf, chain=ch, max_depth=7)
    lf, ch = chain_of(2, [0, None])
    bad("pathlen-0-one-below", lf, BAD, chain=ch)
    lf, ch = chain_of(2, [1, 0])
    good("pathlen-1-one-below", lf, chain=ch)
    lf, ch = chain_of(3, [1, None, None])
    bad("pathlen-1-two-below", lf, BAD, chain=ch)
    lf, ch = chain_of(3, [1, None, None], self_issued_at=1)
    good("pathlen-self-issued-not-counted", lf, chain=ch)
    rootpl = root.but(exts=ca_exts(0))
    lf, ch = chain_of(1)
    bad("root-pathlen-0-one-below", lf, BAD, chain=ch, roots=(rootpl,))  # a root's constraints hold too
    good("root-pathlen-0-none-below", leaf, roots=(rootpl,))
    good("root-pathlen-1-one-below", lf, chain=ch, roots=(root.but(exts=ca_exts(1)),))
    lf, ch = chain_of(2)
    bad("root-pathlen-0-two-below", lf, BAD, chain=ch, roots=(rootpl,))
    # Two sent CAs that sign each other must not send the search round in circles.
    a_key, b_key = ec_key(0x5EED0200), ec_key(0x5EED0201)
    a_stub = Cert(name(("CN", "A")), a_key, exts=ca_exts())
    b = Cert(name(("CN", "B")), b_key, a_stub, exts=ca_exts())
    a = Cert(name(("CN", "A")), a_key, b, exts=ca_exts())
    bad("loop", Cert(leaf.f["subject"], leaf_key, a, exts=leaf_exts()), UNKNOWN_CA, chain=(a, b))

    # Name constraints, on an intermediate.
    def under_nc(*constraint, names=None, nc_ext=None):
        ca = Cert(name(("CN", "Constrained")), ec_key(0x5EED0300), root,
                  exts=ca_exts() + [nc_ext if nc_ext is not None else nc(*constraint)])
        return Cert(leaf.f["subject"] if names is None or names[0] != "empty" else seq(), leaf_key, ca,
                    exts=[san(*(names[1:] if names and names[0] == "empty" else names or
                                (dns("example.com"), ip(192, 0, 2, 1))),
                              critical=bool(names and names[0] == "empty")), ku("digitalSignature")]), (ca,)

    lf, ch = under_nc([dns("example.com")])
    good("nc-dns-permitted", lf, chain=ch)  # the IP address is under no constraint
    lf, ch = under_nc([dns("example.com")], names=[dns("example.com"), dns("www.example.com")])
    good("nc-dns-permitted-ok", lf, chain=ch)
    lf, ch = under_nc([dns("example.com")], names=[dns("example.com"), dns("www.example.org")])
    bad("nc-dns-permitted-one-out", lf, BAD, chain=ch)
    lf, ch = under_nc([dns("example.com")], names=[dns("example.com"), dns("notexample.com")])
    bad("nc-dns-permitted-suffix-not-label", lf, BAD, chain=ch)
    lf, ch = under_nc([], [dns("bad.example.com")], names=[dns("example.com"), dns("x.bad.example.com")])
    bad("nc-dns-excluded", lf, BAD, chain=ch)
    lf, ch = under_nc([], [dns("bad.example.com")], names=[dns("example.com"), dns("*.example.com")])
    bad("nc-dns-excluded-wildcard-reaches", lf, BAD, chain=ch)
    lf, ch = under_nc([], [dns("x.bad.example.com")], names=[dns("example.com"), dns("*.example.com")])
    good("nc-dns-excluded-wildcard-does-not-reach", lf, chain=ch)
    lf, ch = under_nc([dns("EXAMPLE.com")], names=[dns("Www.Example.COM")])
    good("nc-dns-case", lf, chain=ch, host="www.example.com")
    lf, ch = under_nc([ip(192, 0, 2, 0, 255, 255, 255, 0)], names=[ip(192, 0, 2, 1), dns("example.com")])
    good("nc-ip-permitted", lf, chain=ch)
    lf, ch = under_nc([ip(192, 0, 3, 0, 255, 255, 255, 0)], names=[ip(192, 0, 2, 1), dns("example.com")])
    bad("nc-ip-permitted-out", lf, BAD, chain=ch)
    lf, ch = under_nc([], [ip(192, 0, 2, 0, 255, 255, 255, 128)], names=[ip(192, 0, 2, 1), dns("example.com")])
    bad("nc-ip-excluded", lf, BAD, chain=ch)
    lf, ch = under_nc([], [ip(192, 0, 2, 128, 255, 255, 255, 128)], names=[ip(192, 0, 2, 1), dns("example.com")])
    good("nc-ip-excluded-other-half", lf, chain=ch)
    lf, ch = under_nc([ip(*([0x20, 0x01] + [0] * 14 + [0xff, 0xff] + [0] * 14))],
                      names=[ip(192, 0, 2, 1), dns("example.com")])
    bad("nc-ip-other-family-permits-none", lf, BAD, chain=ch)  # IPv4 and IPv6 are one kind of name
    lf, ch = under_nc([dirname(name(("CN", "example.com")))], names=[dns("example.com")])
    good("nc-dn-permitted", lf, chain=ch)
    lf, ch = under_nc([dirname(name(("CN", "elsewhere")))], names=[dns("example.com")])
    bad("nc-dn-permitted-out", lf, BAD, chain=ch)
    lf, ch = under_nc([dirname(name(("CN", "elsewhere")))], names=["empty", dns("example.com")])
    good("nc-dn-empty-subject", lf, chain=ch)
    lf, ch = under_nc([], [dirname(name(("CN", "example.com")))], names=[dns("example.com")])
    bad("nc-dn-excluded", lf, BAD, chain=ch)
    lf, ch = under_nc([email("example.com")], names=[dns("example.com"), email("a@example.com")])
    bad("nc-email-unchecked", lf, UNSUPPORTED, chain=ch)
    lf, ch = under_nc([email("example.com")], names=[dns("example.com")])
    good("nc-email-no-emails", lf, chain=ch)
    for cid, g in [("ip-mask-gap", ip(192, 0, 2, 0, 255, 0, 255, 0)), ("ip-host-bits", ip(192, 0, 2, 1, 255, 255, 255, 0)),
                   ("ip-9", ip(192, 0, 2, 0, 255, 255, 255, 0, 0)), ("ip-4", ip(192, 0, 255, 0)),
                   ("ip-mask-gap-in-byte", ip(192, 0, 2, 0, 255, 255, 255, 0xA0)), ("dns-bad", dns("a_b.example")),
                   ("dn-trailing", tlv(0xA4, name(("CN", "x")) + NULL))]:
        lf, ch = under_nc([g], names=[dns("example.com")])
        bad("nc-parse-" + cid, lf, UNKNOWN_CA, chain=ch)  # an unreadable CA is no way up
    lf, ch = under_nc(nc_ext=ext("2.5.29.30", seq(), True), names=[dns("example.com")])
    bad("nc-parse-empty", lf, UNKNOWN_CA, chain=ch)
    lf, ch = under_nc(nc_ext=ext("2.5.29.30", seq(explicit(0, seq(dns("example.com"), implicit(0, b"\x00")))), True),
                      names=[dns("example.com")])
    bad("nc-parse-minimum", lf, UNKNOWN_CA, chain=ch)
    lf, ch = under_nc(nc_ext=ext("2.5.29.30", seq(explicit(1, seq(dns("x.example"))), explicit(0, seq(dns("example.com"))))),
                      names=[dns("example.com")])
    bad("nc-parse-order", lf, UNKNOWN_CA, chain=ch)
    lf, ch = under_nc([dns("")], names=[dns("example.com")])
    good("nc-dns-empty-permits-all", lf, chain=ch)
    return cases

#!/usr/bin/env python3
"""Writes sig_cases.h: SHA-384/512 answers, and RSA (PKCS#1 v1.5, PSS) and
ECDSA (P-256, P-384) signatures with whether each must verify. Checked in,
so test_sig needs only a compiler; rerun this only to change the cases.

    python3 tests/gen_sig_cases.py WYCHEPROOF_DIR

Most cases are Project Wycheproof's (github.com/C2SP/wycheproof, Apache
2.0; WYCHEPROOF_DIR is its testvectors_v1 directory): signatures built to
hit the arithmetic's corners (points that double in the middle of u1 G +
u2 Q, r and s at the ends of their range, hashes that come out as 0 or
n) and the encodings' (padding that BER would accept, DigestInfo with the
NULL left out, integers with a needless zero). The rest are signed here by
OpenSSL, through Python's `cryptography`, for keys Wycheproof's sizes
miss: RSA moduli whose length in bits is 1 more than a multiple of 8,
where PSS's encoded message is a byte shorter than the modulus."""
import hashlib, json, os, random, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

random.seed(8017)
rb = lambda n: bytes(random.getrandbits(8) for _ in range(n))
HASH = {"SHA-256": 1, "SHA-384": 2, "SHA-512": 3}
HOBJ = {1: hashes.SHA256(), 2: hashes.SHA384(), 3: hashes.SHA512()}
KIND_PKCS1, KIND_PSS, KIND_ECDSA = 1, 2, 3
CURVE = {"secp256r1": 1, "secp384r1": 2}

ECDSA_FILES = ["ecdsa_secp256r1_sha256_test.json", "ecdsa_secp256r1_sha512_test.json",
               "ecdsa_secp384r1_sha256_test.json", "ecdsa_secp384r1_sha384_test.json"]
PKCS1_FILES = ["rsa_signature_2048_sha256_test.json", "rsa_signature_4096_sha512_test.json"]
PSS_FILES = ["rsa_pss_2048_sha256_mgf1_32_test.json", "rsa_pss_2048_sha384_mgf1_48_test.json",
             "rsa_pss_4096_sha512_mgf1_64_test.json"]

keys, cases = [], []  # keys: (a, b, spki): RSA n, e; EC point, curve


def key(a, b, spki):
    keys.append((a, b, spki))
    return len(keys) - 1


def case(kind, hash_, param, k, msg, sig, ok, label):
    cases.append((kind, hash_, param, k, msg, sig, ok, label))


def wycheproof(wp):
    for name in ECDSA_FILES + PKCS1_FILES + PSS_FILES:
        j = json.load(open(os.path.join(wp, name)))
        short = name.replace("_test.json", "")
        for g in j["testGroups"]:
            pk, h = g["publicKey"], HASH[g["sha"]]
            spki = bytes.fromhex(g["publicKeyDer"])
            if name.startswith("ecdsa"):
                kind, param = KIND_ECDSA, CURVE[pk["curve"]]
                k = key(bytes.fromhex(pk["uncompressed"]), b"", spki)
            else:
                kind = KIND_PSS if "pss" in name else KIND_PKCS1
                if kind == KIND_PSS:
                    assert g["mgf"] == "MGF1" and g["mgfSha"] == g["sha"]
                param = g.get("sLen", 0)
                k = key(bytes.fromhex(pk["modulus"]), bytes.fromhex(pk["publicExponent"]), spki)
            for t in g["tests"]:
                # "acceptable" is a PKCS#1 DigestInfo without its NULL: RFC
                # 8017 says to accept only the encoding with it, so refused.
                ok = t["result"] == "valid"
                case(kind, h, param, k, bytes.fromhex(t["msg"]), bytes.fromhex(t["sig"]), ok,
                     "%s #%d" % (short, t["tcId"]))


def prime(bits):
    while True:
        c = random.getrandbits(bits) | 1 << (bits - 1) | 1
        if all(pow(a, c - 1, c) == 1 for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37)):
            return c


def rsa_key(bits):
    """A key whose modulus is exactly this many bits long (OpenSSL, asked
    for 2049, makes 2048)."""
    while True:
        p, q = prime((bits + 1) // 2), prime(bits // 2)
        n = p * q
        if p != q and n.bit_length() == bits and (p - 1) % 65537 and (q - 1) % 65537:
            break
    d = pow(65537, -1, (p - 1) * (q - 1))
    pub = rsa.RSAPublicNumbers(65537, n)
    return rsa.RSAPrivateNumbers(p, q, d, d % (p - 1), d % (q - 1), pow(q, -1, p), pub).private_key()


def mgf1(seed, n, h):
    out = b""
    for i in range((n + HOBJ[h].digest_size - 1) // HOBJ[h].digest_size):
        out += hashlib.new(HOBJ[h].name, seed + i.to_bytes(4, "big")).digest()
    return out[:n]


def pss_encode(m, h, em_bits, salt):
    """EMSA-PSS-ENCODE (RFC 8017 9.1.1), as a number."""
    H = lambda b: hashlib.new(HOBJ[h].name, b).digest()
    hl, em_len = HOBJ[h].digest_size, (em_bits + 7) // 8
    hh = H(b"\0" * 8 + H(m) + salt)
    db = b"\0" * (em_len - len(salt) - hl - 2) + b"\1" + salt
    masked = bytes(a ^ b for a, b in zip(db, mgf1(hh, len(db), h)))
    em = int.from_bytes(masked + hh + b"\xbc", "big")
    return em & ((1 << em_bits) - 1)


def ours():
    # RSA keys of awkward lengths, signed both ways, then broken.
    for bits in (2049, 2056, 3073, 4095, 4096):
        priv = rsa_key(bits)
        pub = priv.public_key()
        nums = pub.public_numbers()
        n = nums.n.to_bytes((bits + 7) // 8, "big")
        e = nums.e.to_bytes(3, "big")
        spki = pub.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        k = key(n, e, spki)
        for h in (1, 2, 3):
            hl = HOBJ[h].digest_size
            for i in range(3):
                m = rb(random.randrange(200))
                s = priv.sign(m, padding.PKCS1v15(), HOBJ[h])
                case(KIND_PKCS1, h, 0, k, m, s, True, "pkcs1 %d bits %s" % (bits, HOBJ[h].name))
                case(KIND_PKCS1, h, 0, k, m + b"x", s, False, "pkcs1 %d bits, other message" % bits)
                pss = padding.PSS(mgf=padding.MGF1(HOBJ[h]), salt_length=hl)
                s = priv.sign(m, pss, HOBJ[h])
                case(KIND_PSS, h, hl, k, m, s, True, "pss %d bits %s" % (bits, HOBJ[h].name))
                b = bytearray(s)
                b[random.randrange(len(b))] ^= 1 << random.randrange(8)
                case(KIND_PSS, h, hl, k, m, bytes(b), False, "pss %d bits, a bit flipped" % bits)
                case(KIND_PSS, h, hl - 1, k, m, s, False, "pss %d bits, salt length 1 short" % bits)
                if h != 3:
                    case(KIND_PSS, 3 if h == 1 else 1, hl, k, m, s, False, "pss %d bits, other hash" % bits)
    # PSS encoded by hand, to reach what OpenSSL never makes: a nonzero
    # byte above the encoded message (moduli of 8k + 1 bits, where it is a
    # byte shorter than the modulus) and a bit set above emBits (others).
    for bits in (2049, 2056, 4095):
        priv = rsa_key(bits)
        nums = priv.private_numbers()
        n, d = nums.public_numbers.n, nums.d
        k = key(n.to_bytes((bits + 7) // 8, "big"), (65537).to_bytes(3, "big"),
                priv.public_key().public_bytes(serialization.Encoding.DER,
                                               serialization.PublicFormat.SubjectPublicKeyInfo))
        size = (bits + 7) // 8
        for h in (1, 3):
            m = rb(50)
            case(KIND_PSS, h, HOBJ[h].digest_size, k, m, pow(pss_encode(m, h, bits - 1, rb(HOBJ[h].digest_size)), d, n)
                 .to_bytes(size, "big"), True, "pss by hand, %d bits" % bits)
            em_bits = bits - 1
            em_len = (em_bits + 7) // 8
            while True:
                em = pss_encode(m, h, em_bits, rb(HOBJ[h].digest_size))
                bad = em | (1 << 8 * em_len) if em_len < size else em | (1 << em_bits)
                if bad < n:
                    break
            what = "a nonzero byte above it" if em_len < size else "a bit set above emBits"
            case(KIND_PSS, h, HOBJ[h].digest_size, k, m, pow(bad, d, n).to_bytes(size, "big"), False,
                 "pss by hand, %d bits, %s" % (bits, what))
    # ECDSA with every hash on both curves, and the same signature with s
    # replaced by n - s (which also verifies: ECDSA is malleable).
    for cname, curve, order in (("secp256r1", ec.SECP256R1(),
                                 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551),
                                ("secp384r1", ec.SECP384R1(),
                                 0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973)):
        priv = ec.generate_private_key(curve)
        pub = priv.public_key()
        point = pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        spki = pub.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        k = key(point, b"", spki)
        for h in (1, 2, 3):
            for i in range(4):
                m = rb(random.randrange(200))
                s = priv.sign(m, ec.ECDSA(HOBJ[h]))
                case(KIND_ECDSA, h, CURVE[cname], k, m, s, True, "ecdsa %s %s" % (cname, HOBJ[h].name))
                r, sv = decode_dss_signature(s)
                case(KIND_ECDSA, h, CURVE[cname], k, m, encode_dss_signature(r, order - sv), True,
                     "ecdsa %s, n - s" % cname)
                case(KIND_ECDSA, h, CURVE[cname], k, m + b"x", s, False, "ecdsa %s, other message" % cname)
        # The key moved off the curve (y + 1), with a signature good for
        # the real key: refused.
        size = len(point) // 2
        y = (int.from_bytes(point[1 + size:], "big") + 1).to_bytes(size, "big")
        k_off = key(point[:1 + size] + y, b"", b"")
        m = b"off the curve"
        case(KIND_ECDSA, 1, CURVE[cname], k_off, m, priv.sign(m, ec.ECDSA(hashes.SHA256())), False,
             "ecdsa %s, a point off the curve" % cname)
        # The same key compressed: refused as unsupported (test_sig checks).
        comp = pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.CompressedPoint)
        k = key(comp, b"", b"")
        m = b"compressed"
        case(KIND_ECDSA, 1, CURVE[cname], k, m, priv.sign(m, ec.ECDSA(hashes.SHA256())), False,
             "ecdsa %s, a compressed point" % cname)


def main():
    wp = sys.argv[1]
    wycheproof(wp)
    ours()
    out = ["// Generated by gen_sig_cases.py. Do not edit.",
           "// Most signature cases are from Project Wycheproof (github.com/C2SP/wycheproof,",
           "// Apache License 2.0); the rest, and the hashes, are answers from OpenSSL.",
           "// Bytes are written as hex.",
           "typedef struct { const char *msg, *digest; } hash_case_t;",
           "typedef struct { const char *a, *b, *spki; } sig_key_t;  // RSA: n, e. EC: the point.",
           "typedef struct { int kind, hash, param, key; const char *msg, *sig; int ok; const char* label; } sig_case_t;",
           "enum { KIND_PKCS1 = 1, KIND_PSS, KIND_ECDSA };"]
    for name, f in (("sha384_cases", hashlib.sha384), ("sha512_cases", hashlib.sha512)):
        out.append("static const hash_case_t %s[] = {" % name)
        for n in list(range(0, 270)) + [511, 512, 513, 1000, 4097]:
            m = rb(n)
            out.append('    {"%s", "%s"},' % (m.hex(), f(m).hexdigest()))
        out.append("};")
    out.append("static const sig_key_t sig_keys[] = {")
    for a, b, spki in keys:
        out.append('    {"%s", "%s", "%s"},' % (a.hex(), b.hex(), spki.hex()))
    out.append("};")
    out.append("static const sig_case_t sig_cases[] = {")
    for kind, h, param, k, msg, sig, ok, label in cases:
        out.append('    {%d, %d, %d, %d, "%s", "%s", %d, "%s"},' % (kind, h, param, k, msg.hex(), sig.hex(), ok, label))
    out.append("};")
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "sig_cases.h"), "w").write("\n".join(out) + "\n")
    print("%d keys, %d signatures (%d valid)" % (len(keys), len(cases), sum(c[6] for c in cases)))


main()

#!/usr/bin/env python3
"""Records the conversations in tls_cases.h, which test_tls replays.

    python3 tests/gen_tls_cases.py [OUT]    (from the repository root;
                                             OUT in place of tests/tls_cases.h)

Our TLS client (src/programs/lib/tls.c) is built for the host and run,
once for each case, against a server on this machine. Three servers:

  * A scripted one, written here from RFC 8446 and sharing no code with
    the client. It can do everything a server may do in ways real ones
    seldom choose to (handshakes cut into seven-byte records, padding,
    key updates, a request to retry) and everything a server must not do.
    It also checks the client: that the hello is what it should be, that
    every record opens under the right key, that the client's Finished is
    right, and that after each broken rule the client sends the alert the
    RFC names.
  * OpenSSL through Python's ssl module.
  * OpenSSL's own s_server program, for what Python cannot make it do:
    update its keys, and ask the client to update its own.

Every case is run with a stand-in for the certificate verifier, which
answers what the case says (most often yes) and writes down what the
client handed it. That is checked here against what the server sent:
the same certificates, and a signature that verifies, with this
library, over what the client says the server signed. So the client's
idea of the handshake transcript is checked against OpenSSL's and the
scripted server's, and any verifier given these inputs is checking the
right thing.

What each case should come to is written down by hand next to it, and a
case is recorded only if the client did exactly that. So the header is
not "what the client happened to do": it is what the RFC says, as far
as it could be checked, in a form that runs anywhere.

The scripted server is deterministic, so its cases come out the same
every time. OpenSSL chooses fresh random numbers, so those cases differ
from run to run (in bytes, not in meaning).

Needs: gcc -m32, Python's cryptography package, the openssl program.
"""
import hashlib
import hmac
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import ssl
import datetime

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, x25519, rsa, padding
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# tls.h's error numbers.
E_EOF, E_NOT_TLS, E_VERSION, E_PROTOCOL, E_DECODE, E_PARAM = -1001, -1002, -1003, -1004, -1005, -1006
E_BAD_MAC, E_FINISHED, E_TOO_BIG, E_NOMEM, E_CLOSED, E_NAME = -1007, -1008, -1009, -1010, -1011, -1012
E_CERT_BAD, E_CERT_UNSUPPORTED, E_CERT_EXPIRED, E_CERT_UNKNOWN_CA = -1020, -1021, -1022, -1023
E_CERT_NAME, E_CERT_SIGNATURE = -1024, -1025
E_ALERT = -2000

# Alerts (RFC 8446 section 6).
CLOSE_NOTIFY, UNEXPECTED_MESSAGE, BAD_RECORD_MAC, RECORD_OVERFLOW = 0, 10, 20, 22
HANDSHAKE_FAILURE, ILLEGAL_PARAMETER, DECODE_ERROR, DECRYPT_ERROR = 40, 47, 50, 51
PROTOCOL_VERSION, INTERNAL_ERROR, USER_CANCELED = 70, 80, 90
MISSING_EXTENSION, UNSUPPORTED_EXTENSION, UNRECOGNIZED_NAME = 109, 110, 112
BAD_CERTIFICATE, UNSUPPORTED_CERTIFICATE, CERTIFICATE_EXPIRED, UNKNOWN_CA = 42, 43, 45, 48

CCS, ALERT, HANDSHAKE, APPDATA = 20, 21, 22, 23
CLIENT_HELLO, SERVER_HELLO, NEW_SESSION_TICKET, ENCRYPTED_EXTENSIONS = 1, 2, 4, 8
CERTIFICATE, CERTIFICATE_REQUEST, CERTIFICATE_VERIFY, FINISHED, KEY_UPDATE = 11, 13, 15, 20, 24

HRR_RANDOM = hashlib.sha256(b'HelloRetryRequest').digest()
# What the client says a signature may be made with: ECDSA P-256 and P-384, RSA-PSS, and RSA
# PKCS#1, which may sign certificates but not the handshake (4.4.3).
SIG_SCHEMES = [0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601]
CV_CONTEXT = b' ' * 64 + b'TLS 1.3, server CertificateVerify\x00'

HOST = 'example.test'
REQUEST = b'GET / HTTP/1.0\r\nHost: example.test\r\n\r\n'
REPLY = b'HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\nhello, tls!\n'


def sha256(b):
    return hashlib.sha256(b).digest()


def vec8(b):
    return bytes([len(b)]) + b


def vec16(b):
    return struct.pack('>H', len(b)) + b


def vec24(b):
    return len(b).to_bytes(3, 'big') + b


def msg(kind, body):
    return bytes([kind]) + vec24(body)


def ext(kind, body):
    return struct.pack('>H', kind) + vec16(body)


# ---- The key schedule (RFC 8446 section 7.1), from the RFC and nothing else ----

def hkdf_extract(salt, ikm):
    return hmac.new(salt, ikm, 'sha256').digest()


def hkdf_expand(prk, info, n):
    out, block, i = b'', b'', 1
    while len(out) < n:
        block = hmac.new(prk, block + info + bytes([i]), 'sha256').digest()
        out += block
        i += 1
    return out[:n]


def expand_label(secret, label, context, n):
    return hkdf_expand(secret, struct.pack('>H', n) + vec8(b'tls13 ' + label) + vec8(context), n)


def retry_transcript(first_hello, retry):
    """The transcript after a HelloRetryRequest: the first hello's place is taken by a made-up
    message holding its hash (section 4.4.1)."""
    return msg(254, sha256(first_hello)) + retry


def handshake_secrets(shared, transcript):
    """The handshake secret and the two handshake traffic secrets, client's then server's (7.1)."""
    early = hkdf_extract(bytes(32), bytes(32))
    secret = hkdf_extract(expand_label(early, b'derived', sha256(b''), 32), shared)
    h = sha256(transcript)
    return secret, expand_label(secret, b'c hs traffic', h, 32), expand_label(secret, b's hs traffic', h, 32)


# No server to hand will ask our client to retry (OpenSSL's s_server -stateless cannot cope with
# the ChangeCipherSpec a client sends before its second hello, its own client's included). So for
# that path the scripted server is the only witness, and it is itself checked here against the
# retried handshake in RFC 8448, section 5: the two hellos, the retry between them and the
# ServerHello, as printed there, must give the handshake secrets printed there.
RFC8448_RETRY = [bytes.fromhex(h) for h in (
    '010000b00303b0b1c5a5aa37c5919f2ed1d5c6fff7fcb7849716945a2b8cee9258a346677b6f000006130113031302010000'
    '810000000b0009000006736572766572ff01000100000a00080006001d00170018003300260024001d0020e8e8e3f3b93a25'
    'ed97a14a7dcacb8a272c6288e585c6484d05262fcad062ad1f002b0003020304000d0020001e040305030603020308040805'
    '080604010501060102010402050206020202002d00020101001c00024001',
    '020000ac0303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c001301000084003300020017'
    '002c0074007271dcd04bb88bc3189119398a00000000eefafc76c146b823b096f8aacad365dd0030953f4edf625636e5f21b'
    'b2e23fcc654b1b5b40318d10d137abcbb87574e36e8a1f025f7dfa5d6e50781b5eda4aa15b0c8be778257d16aa3030e9e784'
    '1dd9e4c0342267e8ca0caf571fb2b7cff0f934b0002b00020304',
    '010001fc0303b0b1c5a5aa37c5919f2ed1d5c6fff7fcb7849716945a2b8cee9258a346677b6f000006130113031302010001'
    'cd0000000b0009000006736572766572ff01000100000a00080006001d001700180033004700450017004104a6da7392ec59'
    '1e17abfd535964b99894d13befb221b3def2ebe3830eac8f0151812677c4d6d2237e85cf01d6910cfb83954e76ba73528305'
    '34159897e8065780002b0003020304000d0020001e0403050306030203080408050806040105010601020104020502060202'
    '02002c0074007271dcd04bb88bc3189119398a00000000eefafc76c146b823b096f8aacad365dd0030953f4edf625636e5f2'
    '1bb2e23fcc654b1b5b40318d10d137abcbb87574e36e8a1f025f7dfa5d6e50781b5eda4aa15b0c8be778257d16aa3030e9e7'
    '841dd9e4c0342267e8ca0caf571fb2b7cff0f934b0002d00020101001c00024001001500af00000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    '000000000000000000000000',
    '020000770303bb341d847fd789c47c387172dc0c9bf147fccacb5043d86ca4c598d3ff571b9800130100004f003300450017'
    '004104583e054b7a66672ae020ad9d2686fcc85b5ad41a134a0f03ee72b893052bd85b4c8de6776f5b04ac07d83540eab3e3'
    'd9c547bc6528c4317d294686093a6cad7d002b00020304')]


def check_against_rfc8448():
    first, retry, second, server_hello = RFC8448_RETRY
    shared = bytes.fromhex('c142ce13ca11b5c2233652e63ad3d97844f1621fbfb9de69d547dc8fedeabeb4')
    _, client, server = handshake_secrets(shared, retry_transcript(first, retry) + second + server_hello)
    assert client.hex() == '158aa7ab8855073582b41d674b4055cabcc534728f659314861b4e08e2011566'
    assert server.hex() == '3403e781e2af7b6508da28574f6e95a1abf162de83a97927c37672a4a0cef8a1'


class Keys:
    """One direction's record protection."""

    def __init__(self, secret, suite=0x1303):
        self.secret, self.suite = secret, suite
        if suite == 0x1301:
            self.aead = AESGCM(expand_label(secret, b'key', b'', 16))
        else:
            self.aead = ChaCha20Poly1305(expand_label(secret, b'key', b'', 32))
        self.iv = int.from_bytes(expand_label(secret, b'iv', b'', 12), 'big')
        self.seq = 0

    def nonce(self):
        n = (self.iv ^ self.seq).to_bytes(12, 'big')
        self.seq += 1
        return n

    def seal(self, header, plain):
        return self.aead.encrypt(self.nonce(), plain, header)

    def open(self, header, sealed):
        return self.aead.decrypt(self.nonce(), sealed, header)

    def next(self):
        return Keys(expand_label(self.secret, b'traffic upd', b'', 32), self.suite)


# ---- The certificate the servers show ----

# Everything here is deterministic, so the scripted cases come out the
# same every time: ECDSA signs as RFC 6979 says, the RSA key is made
# from primes found by a seeded search, and RSA-PSS takes its salt from
# what it signs. (The salt is the signer's business; the verifier cannot
# tell, which is the point of checking with a library that is not ours.)

def probable_prime(n):
    if n % 2 == 0:
        return n == 2
    for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d, r = d // 2, r + 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71):
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def seeded_prime(seed, bits):
    for i in range(100000):
        h = b''.join(hashlib.sha256(seed + b'/%d/%d' % (i, j)).digest() for j in range(bits // 256))
        n = int.from_bytes(h, 'big') | 3 << (bits - 2) | 1
        if (n - 1) % 65537 and probable_prime(n):
            return n
    raise RuntimeError('no prime')


def rsa_key(seed, bits=2048):
    p, q = seeded_prime(seed + b'/p', bits // 2), seeded_prime(seed + b'/q', bits // 2)
    e, n = 65537, p * q
    d = pow(e, -1, (p - 1) * (q - 1))
    return rsa.RSAPrivateNumbers(p, q, d, d % (p - 1), d % (q - 1), pow(q, -1, p),
                                 rsa.RSAPublicNumbers(e, n)).private_key()


PSS_HASH = {0x0804: 'sha256', 0x0805: 'sha384', 0x0806: 'sha512'}
PKCS1_HASH = {0x0401: hashes.SHA256(), 0x0501: hashes.SHA384(), 0x0601: hashes.SHA512()}
ECDSA_SCHEMES = {0x0403: (ec.SECP256R1, hashes.SHA256()), 0x0503: (ec.SECP384R1, hashes.SHA384())}


def pss_sign(key, hname, data):
    """RSASSA-PSS (RFC 8017 9.1.1), MGF1 with the same hash, a salt as long as the hash."""
    def H(b):
        return hashlib.new(hname, b).digest()
    hlen = len(H(b''))
    priv = key.private_numbers()
    n, d = priv.public_numbers.n, priv.d
    em_bits = n.bit_length() - 1
    em_len = (em_bits + 7) // 8
    salt = H(b'salt for ' + data)
    h = H(bytes(8) + H(data) + salt)
    db = bytes(em_len - 2 * hlen - 2) + b'\x01' + salt
    mask = b''.join(H(h + struct.pack('>I', i)) for i in range((len(db) + hlen - 1) // hlen))[:len(db)]
    masked = bytearray(x ^ y for x, y in zip(db, mask))
    masked[0] &= 0xff >> (8 * em_len - em_bits)
    em = int.from_bytes(bytes(masked) + h + b'\xbc', 'big')
    return pow(em, d, n).to_bytes((n.bit_length() + 7) // 8, 'big')


def sign(key, scheme, data):
    if scheme in ECDSA_SCHEMES:
        return key.sign(data, ec.ECDSA(ECDSA_SCHEMES[scheme][1], deterministic_signing=True))
    if scheme in PSS_HASH:
        return pss_sign(key, PSS_HASH[scheme], data)
    if scheme in PKCS1_HASH:
        return key.sign(data, padding.PKCS1v15(), PKCS1_HASH[scheme])
    return sha256(b'not a signature by any scheme we know' + data) * 2


def verify(cert_der, scheme, data, sig):
    """Whether sig is a good signature over data by the key in the certificate, in this scheme,
    as Python's cryptography (OpenSSL underneath) sees it."""
    try:
        key = x509.load_der_x509_certificate(cert_der).public_key()
        if scheme in ECDSA_SCHEMES:
            curve, h = ECDSA_SCHEMES[scheme]
            if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(key.curve, curve):
                return False
            key.verify(sig, data, ec.ECDSA(h))
        elif scheme in PSS_HASH:
            h = {'sha256': hashes.SHA256(), 'sha384': hashes.SHA384(), 'sha512': hashes.SHA512()}[PSS_HASH[scheme]]
            if not isinstance(key, rsa.RSAPublicKey):
                return False
            key.verify(sig, data, padding.PSS(padding.MGF1(h), h.digest_size), h)
        else:
            return False
        return True
    except (InvalidSignature, ValueError):
        return False


def make_certificate(key, h=hashes.SHA256()):
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, HOST)])
    builder = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
               .serial_number(1)
               .not_valid_before(datetime.datetime(2026, 1, 1))
               .not_valid_after(datetime.datetime(2046, 1, 1))
               .add_extension(x509.SubjectAlternativeName([x509.DNSName(HOST)]), critical=False))
    if isinstance(key, ec.EllipticCurvePrivateKey):
        return builder.sign(key, h, ecdsa_deterministic=True)
    return builder.sign(key, h)  # RSA PKCS#1 v1.5, which is deterministic


KEY = ec.derive_private_key(0x52696e675461696c2054c4c5320746573742073657276657220633d31, ec.SECP256R1())
CERT_OBJECT = make_certificate(KEY)
CERT = CERT_OBJECT.public_bytes(serialization.Encoding.DER)
KEY384 = ec.derive_private_key(0x52696e675461696c20502d333834, ec.SECP384R1())
CERT384 = make_certificate(KEY384, hashes.SHA384()).public_bytes(serialization.Encoding.DER)
RSA_KEY = rsa_key(b'RingTail TLS test RSA key')
RSA_CERT = make_certificate(RSA_KEY).public_bytes(serialization.Encoding.DER)


class Wrong(Exception):
    """The client did something other than what the case says it should."""


def need(ok, what):
    if not ok:
        raise Wrong(what)


# ---- The scripted server ----

class Server:
    def __init__(self, sock, case):
        self.sock, self.case = sock, case
        self.sent, self.got = bytearray(), bytearray()
        self.buf, self.eof = b'', False
        self.counter = 0
        self.transcript = b''
        self.rd = self.wr = None  # Keys, once there are any
        self.suite = 0x1303  # the cipher suite chosen; a case may choose the other
        self.hs_in = b''
        self.records_in = 0
        self.ccs_in = 0
        self.sent_certs = None  # the certificates as sent, when m_cert made the message
        self.before_cv = None   # the transcript hash under the CertificateVerify
        self.cv = None          # and that message
        self.strict = False  # the scenario accounted for everything the client should send
        self.private = x25519.X25519PrivateKey.from_private_bytes(self.random(32))
        self.public = self.private.public_key().public_bytes(serialization.Encoding.Raw,
                                                             serialization.PublicFormat.Raw)

    def random(self, n):
        out = b''
        while len(out) < n:
            out += sha256(b'scripted server/' + self.case.name.encode() + b'/%d' % self.counter)
            self.counter += 1
        return out[:n]

    # -- bytes in and out --

    def raw(self, data):
        self.sent += data
        try:
            self.sock.sendall(data)
        except OSError:
            pass  # the client has gone; it never saw these, and a replay will not read them either

    def fill(self):
        try:
            data = self.sock.recv(65536)
        except ConnectionResetError:
            data = b''
        if not data:
            self.eof = True
        self.got += data
        self.buf += data

    def read_record(self):
        """The client's next record as (type, contents), opened; None at the end."""
        while True:
            while len(self.buf) < 5 and not self.eof:
                self.fill()
            if not self.buf and self.eof:
                return None
            need(len(self.buf) >= 5, 'the client stopped in the middle of a record header')
            kind, version, n = struct.unpack('>BHH', self.buf[:5])
            while len(self.buf) < 5 + n and not self.eof:
                self.fill()
            need(len(self.buf) >= 5 + n, 'the client stopped in the middle of a record')
            header, body, self.buf = self.buf[:5], self.buf[5:5 + n], self.buf[5 + n:]
            # The first record says 3.1 for old servers' sake, every later one 3.3 (section 5.1).
            need(version == (0x0301 if self.records_in == 0 else 0x0303), 'record version %04x' % version)
            self.records_in += 1
            if kind == CCS:
                need(body == b'\x01', 'a ChangeCipherSpec that is not the one byte 1')
                self.ccs_in += 1
                continue
            if self.rd is None:
                need(kind in (ALERT, HANDSHAKE), 'record type %d before there are keys' % kind)
                need(n <= 16384, 'a record over the limit')
                return kind, body
            need(kind == APPDATA, 'a record in the clear (type %d) when there are keys' % kind)
            need(n <= 16384 + 256, 'a sealed record over the limit')
            try:
                plain = self.rd.open(header, body)
            except Exception:
                raise Wrong('a record from the client does not open under the key it should be under')
            plain = plain.rstrip(b'\x00')
            need(plain, 'a sealed record with nothing in it')
            need(len(plain) - 1 <= 16384, 'more than 16384 bytes in a record')
            return plain[-1], plain[:-1]

    def read_handshake(self):
        """The client's next handshake message as (type, body, all of it)."""
        while len(self.hs_in) < 4 or len(self.hs_in) < 4 + int.from_bytes(self.hs_in[1:4], 'big'):
            record = self.read_record()
            need(record is not None, 'the client hung up when a handshake message was due')
            need(record[0] == HANDSHAKE and record[1], 'record type %d when a handshake message was due' % record[0])
            self.hs_in += record[1]
        n = 4 + int.from_bytes(self.hs_in[1:4], 'big')
        whole, self.hs_in = self.hs_in[:n], self.hs_in[n:]
        return whole[0], whole[4:], whole

    def plain(self, kind, payload, version=0x0303):
        self.raw(struct.pack('>BHH', kind, version, len(payload)) + payload)

    def sealed(self, kind, payload, pad=0, outer=APPDATA, damage=None, keys=None, inner=None):
        """One sealed record. damage: a function from the sealed bytes to worse ones."""
        keys = keys or self.wr
        text = inner if inner is not None else payload + bytes([kind]) + bytes(pad)
        header = struct.pack('>BHH', outer, 0x0303, len(text) + 16)
        body = keys.seal(header, text)
        if damage:
            body = damage(body)
        self.raw(header + body)

    def send(self, kind, payload, size=16384, pad=0):
        """payload in records of at most `size` bytes (or of each size in a list, round and round)."""
        sizes = size if isinstance(size, list) else [size]
        at = i = 0
        while True:
            piece = payload[at:at + sizes[i % len(sizes)]]
            if self.wr:
                self.sealed(kind, piece, pad)
            else:
                self.plain(kind, piece)
            at += len(piece)
            i += 1
            if at >= len(payload):
                break

    # -- the client's hello --

    def client_hello(self, cookie=None):
        """Reads a ClientHello and holds it to the letter of section 4.1.2."""
        kind, body, whole = self.read_handshake()
        need(kind == CLIENT_HELLO, 'the first message is not ClientHello')
        need(not self.hs_in, 'bytes after the ClientHello')
        at = 0

        def take(n):
            nonlocal at
            need(at + n <= len(body), 'the ClientHello is cut short')
            at += n
            return body[at - n:at]

        need(take(2) == b'\x03\x03', 'legacy_version is not 0x0303')
        hello = {'whole': whole, 'random': take(32)}
        hello['sid'] = take(take(1)[0])
        need(len(hello['sid']) == 32, 'the session id is not 32 bytes')
        need(take(int.from_bytes(take(2), 'big')) == b'\x13\x03\x13\x01', 'cipher suites other than the two')
        need(take(take(1)[0]) == b'\x00', 'compression methods other than "none"')
        exts = take(int.from_bytes(take(2), 'big'))
        need(at == len(body), 'bytes after the extensions')
        found, at2 = {}, 0
        while at2 < len(exts):
            need(at2 + 4 <= len(exts), 'an extension cut short')
            t, n = struct.unpack('>HH', exts[at2:at2 + 4])
            need(at2 + 4 + n <= len(exts), 'an extension cut short')
            need(t not in found, 'extension %d twice' % t)
            found[t] = exts[at2 + 4:at2 + 4 + n]
            at2 += 4 + n
        host = self.case.sni()
        want = {43, 10, 13, 51} | ({0} if host else set()) | ({44} if cookie is not None else set())
        need(set(found) == want, 'extensions %s, not %s' % (sorted(found), sorted(want)))
        if host:
            h = host.encode()
            need(found[0] == vec16(b'\x00' + vec16(h)), 'server_name is not %r' % host)
        need(found[43] == vec8(b'\x03\x04'), 'supported_versions is not just TLS 1.3')
        need(found[10] == vec16(b'\x00\x1d'), 'supported_groups is not just x25519')
        need(found[13] == vec16(b''.join(struct.pack('>H', s) for s in SIG_SCHEMES)), 'signature_algorithms')
        need(found[51][:6] == struct.pack('>HHH', 36, 0x1d, 32) and len(found[51]) == 38, 'key_share')
        hello['share'] = found[51][6:]
        if cookie is not None:
            need(found[44] == vec16(cookie), 'the cookie did not come back as it went')
        # Random numbers that are zeros or the same thing three times would be worth knowing about.
        need(len({hello['random'], hello['sid'], hello['share']}) == 3, 'the hello repeats its random numbers')
        need(max(len(set(hello[k])) for k in ('random', 'sid', 'share')) > 8, 'random numbers that are not')
        return hello

    # -- the server's hello --

    def share(self, public=None):
        return ext(51, struct.pack('>H', 0x1d) + vec16(self.public if public is None else public))

    def make_hello(self, hello, version=0x0303, random=None, sid=None, suite=None, comp=0, exts=None, tail=b'',
                   bare=False):
        suite = self.suite if suite is None else suite
        if exts is None:
            exts = ext(43, b'\x03\x04') + self.share()
        elif callable(exts):
            exts = exts(self)
        body = (struct.pack('>H', version) + (self.random(32) if random is None else random) +
                vec8(hello['sid'] if sid is None else sid) + struct.pack('>HB', suite, comp))
        if not bare:
            body += vec16(exts)
        return msg(SERVER_HELLO, body + tail)

    def make_retry(self, hello, cookie=b'come back with this', exts=None, suite=None):
        if exts is None:
            exts = ext(43, b'\x03\x04') + ext(44, vec16(cookie))
        return self.make_hello(hello, random=HRR_RANDOM, exts=exts, suite=suite)

    def retry(self, hello, cookie=b'come back with this', suite=None):
        """HelloRetryRequest and the second ClientHello (section 4.1.4)."""
        retry = self.make_retry(hello, cookie, suite=suite)
        self.transcript = retry_transcript(hello['whole'], retry)
        self.plain(HANDSHAKE, retry)
        again = self.client_hello(cookie)
        need(self.ccs_in == 1, 'no ChangeCipherSpec before the second ClientHello')
        for k in ('random', 'sid', 'share'):
            need(again[k] == hello[k], 'the second ClientHello changed its ' + k)
        self.transcript += again['whole']
        return again

    def hello(self, keys=True, retry=None, retry_suite=None, **tweaks):
        """retry_suite: the suite the HelloRetryRequest names, when not the one chosen after it."""
        if tweaks.get('suite') in (0x1301, 0x1303):
            self.suite = tweaks.pop('suite')
        hello = self.client_hello()
        self.transcript = hello['whole']
        if retry is not None:
            hello = self.retry(hello, retry, retry_suite)
        sh = self.make_hello(hello, **tweaks)
        self.transcript += sh
        self.plain(HANDSHAKE, sh)
        if keys:
            self.handshake_keys(hello)
        return hello

    def handshake_keys(self, hello):
        shared = self.private.exchange(x25519.X25519PublicKey.from_public_bytes(hello['share']))
        self.hs_secret, client, server = handshake_secrets(shared, self.transcript)
        self.rd, self.wr = Keys(client, self.suite), Keys(server, self.suite)

    def ccs(self, body=b'\x01'):
        self.plain(CCS, body)

    # -- the rest of the server's flight --

    def m_ee(self, exts=b''):
        return msg(ENCRYPTED_EXTENSIONS, vec16(exts))

    def m_creq(self, context=b'', exts=None):
        if exts is None:
            exts = ext(13, vec16(b'\x04\x03\x08\x04'))
        return msg(CERTIFICATE_REQUEST, vec8(context) + vec16(exts))

    def m_cert(self, certs=None, context=b''):
        entries = b''
        self.sent_certs = []
        for c in ([CERT] if certs is None else certs):
            der, exts = c if isinstance(c, tuple) else (c, b'')
            entries += vec24(der) + vec16(exts)
            self.sent_certs.append(der)
        return msg(CERTIFICATE, vec8(context) + vec24(entries))

    def m_cv(self, key=None, scheme=0x0403):
        # The signature is over the handshake up to here (4.4.3).
        signed = CV_CONTEXT + sha256(self.transcript)
        sig = sign(key or KEY, scheme, signed)
        return msg(CERTIFICATE_VERIFY, struct.pack('>H', scheme) + vec16(sig))

    def finished_mac(self, secret):
        return hmac.new(expand_label(secret, b'finished', b'', 32), sha256(self.transcript), 'sha256').digest()

    def m_fin(self):
        return msg(FINISHED, self.finished_mac(self.wr.secret))

    def flight(self, order=('ee', 'cert', 'cv', 'fin'), layout='each', pad=0, certs=None, signer=None, **instead):
        """The server's sealed flight. instead: a message's name to other bytes (or a function giving
        them, called when its turn comes) to send in its place. certs: the certificates to send;
        signer: the key and scheme to sign with."""
        need(set(instead) <= set(order), 'the case names a message the flight does not have: %r' % (instead,))
        msgs = []
        for name in order:
            m = instead.get(name)
            if name == 'cv':
                self.before_cv = sha256(self.transcript)
            if m is None:
                m = (self.m_cert(certs) if name == 'cert' else self.m_cv(*signer) if name == 'cv' and signer
                     else getattr(self, 'm_' + name)())
            elif callable(m):
                m = m()
            if name == 'cv':
                self.cv = m
            self.transcript += m
            msgs.append(m)
        if layout == 'each':
            for m in msgs:
                self.send(HANDSHAKE, m, pad=pad)
        elif layout == 'one':
            self.send(HANDSHAKE, b''.join(msgs), pad=pad)
        else:
            self.send(HANDSHAKE, b''.join(msgs), size=layout, pad=pad)

    def app_keys(self):
        """The server's side moves to the application key after its Finished (7.1)."""
        master = hkdf_extract(expand_label(self.hs_secret, b'derived', sha256(b''), 32), bytes(32))
        h = sha256(self.transcript)
        self.c_ap = expand_label(master, b'c ap traffic', h, 32)
        self.wr = Keys(expand_label(master, b's ap traffic', h, 32), self.suite)

    def client_flight(self, certificate=False):
        """The client's ChangeCipherSpec, its empty Certificate if asked for one, and its Finished."""
        if certificate:
            kind, body, whole = self.read_handshake()
            need(kind == CERTIFICATE and body == b'\x00\x00\x00\x00', 'not an empty Certificate')
            self.transcript += whole
        want = self.finished_mac(self.rd.secret)
        kind, body, whole = self.read_handshake()
        need(kind == FINISHED, 'message %d where the client\'s Finished should be' % kind)
        need(body == want, 'the client\'s Finished is wrong')
        need(not self.hs_in, 'bytes after the client\'s Finished')
        need(self.ccs_in == 1, 'the client sent %d ChangeCipherSpecs, not one' % self.ccs_in)
        self.rd = Keys(self.c_ap, self.suite)

    def handshake(self, ccs=True, creq=False, early=None, **kw):
        """A whole handshake that a client should accept. early: data sent before the client's
        Finished has come, as a server may (section 2)."""
        hello_kw = {k: kw.pop(k) for k in ('retry', 'suite', 'retry_suite') if k in kw}
        self.hello(**hello_kw)
        if ccs:
            self.ccs()
        if creq:
            kw.setdefault('order', ('ee', 'creq', 'cert', 'cv', 'fin'))
        self.flight(**kw)
        self.app_keys()
        if early:
            self.data(early)
        self.client_flight(certificate=creq)

    # -- after the handshake --

    def request(self, want=None):
        """Reads what the client sends, which must be the case's request."""
        want = self.case.request if want is None else want
        got = b''
        while len(got) < len(want):
            record = self.read_record()
            need(record is not None, 'the client hung up before sending its request')
            need(record[0] == APPDATA, 'record type %d where the request should be' % record[0])
            got += record[1]
        need(got == want, 'the request did not arrive as it was sent')

    def data(self, payload, size=16384, pad=0):
        self.send(APPDATA, payload, size, pad)

    def ticket(self):
        return msg(NEW_SESSION_TICKET, struct.pack('>II', 7200, 0x01020304) + vec8(b'\x00\x01') +
                   vec16(self.random(48)) + vec16(b''))

    def key_update(self, ask=False):
        """The server moves to its next key; if it asks, the client must too, at once (4.6.3)."""
        self.send(HANDSHAKE, msg(KEY_UPDATE, bytes([1 if ask else 0])))
        self.wr = self.wr.next()
        if ask:
            kind, body, _ = self.read_handshake()
            need(kind == KEY_UPDATE and body == b'\x00', 'no KeyUpdate in answer to ours')
            need(not self.hs_in, 'bytes after the KeyUpdate')
            self.rd = self.rd.next()

    def bye(self):
        self.send(ALERT, bytes([1, CLOSE_NOTIFY]))

    def expect_bye(self):
        record = self.read_record()
        need(record == (ALERT, bytes([1, CLOSE_NOTIFY])), 'no close_notify from the client, but %r' % (record,))
        self.strict = True

    def alert(self, description):
        """The client must now send this fatal alert, and nothing after it."""
        record = self.read_record()
        need(record is not None, 'the client hung up without the alert (%d) it owes' % description)
        need(record[0] == ALERT, 'record type %d where an alert (%d) should be' % (record[0], description))
        need(record[1] == bytes([2, description]),
             'alert %s where a fatal %d should be' % (list(record[1]), description))
        self.strict = True

    def silent(self):
        """The client must send nothing more at all."""
        self.strict = True

    def finish(self):
        """Hangs up, and reads whatever else the client sends."""
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        tail = []
        while True:
            record = self.read_record()
            if record is None:
                break
            tail.append(record)
        if self.strict:
            need(not tail, 'the client sent more than it should: %r' % (tail,))


# ---- The cases ----

class Case:
    """verdict: what the stand-in verifier answers. sig_good: whether the server's signature is one
    (a case that sends a bad one says so, and the check here must agree)."""
    def __init__(self, name, run, connect=0, final=0, data=b'', host=HOST, request=REQUEST, again=b'', abuse=False,
                 verdict=0, sig_good=True):
        self.name, self.run = name, run
        self.connect, self.final, self.data = connect, final, data
        self.host, self.request, self.again, self.abuse = host, request, again, abuse
        self.verdict, self.sig_good = verdict, sig_good
        self.seed = zlib.crc32(name.encode()) & 0x7fffffff

    def sni(self):
        """The name the hello should carry: none for an address, and without a last dot."""
        h = self.host.rstrip('.')
        if not h or all(p.isdigit() for p in h.split('.')):
            return None
        return h


CASES = []


def case(name, run, **kw):
    CASES.append(Case(name, run, **kw))


def good(name, run=None, data=REPLY, **kw):
    """A conversation that should work: handshake, request, reply, goodbyes."""
    case_kw = {k: kw.pop(k) for k in ('host', 'request', 'again', 'abuse', 'sig_good') if k in kw}

    def default(S):
        S.handshake(**kw)
        S.request()
        S.data(S.case.data)
        S.bye()
        S.expect_bye()

    case(name, run or default, data=data, **case_kw)


def bad_hello(name, error, alert, **tweaks):
    """A ServerHello the client must refuse, with this error and this alert."""
    def run(S):
        S.hello(keys=False, **tweaks)
        S.alert(alert)
    case(name, run, connect=error)


def bad_first(name, error, alert, send):
    """Something other than a ServerHello in answer to the hello."""
    def run(S):
        hello = S.client_hello()
        send(S, hello)
        if alert is None:
            S.silent()
        else:
            S.alert(alert)
    case(name, run, connect=error)


def bad_flight(name, error, alert, send=None, **kw):
    """A flight the client must refuse. send: what to do in place of the usual flight."""
    case_kw = {k: kw.pop(k) for k in ('verdict', 'sig_good', 'host') if k in kw}
    hello_kw = {k: kw.pop(k) for k in ('suite',) if k in kw}

    def run(S):
        S.hello(**hello_kw)
        S.ccs()
        if send:
            send(S)
        else:
            S.flight(**kw)
        if alert is None:
            S.silent()
        else:
            S.alert(alert)
    case(name, run, connect=error, **case_kw)


def bad_after(name, error, alert, send, data=b''):
    """A good handshake and request, then something the client must refuse."""
    def run(S):
        S.handshake()
        S.request()
        send(S)
        if alert is None:
            S.silent()
        else:
            S.alert(alert)
    case(name, run, final=error, data=data)


SV = ext(43, b'\x03\x04')
ALPN = ext(16, vec16(vec8(b'http/1.1')))


def flip_last(b):
    return b[:-1] + bytes([b[-1] ^ 1])


def flip_first(b):
    return bytes([b[0] ^ 0x80]) + b[1:]


def define_cases():
    # ---- Conversations that should work ----
    good('the plain case', abuse=True)
    good('no ChangeCipherSpec from the server', ccs=False)

    def several_ccs(S):
        S.ccs()  # one before the hello, and three after: all to be ignored (section 5)
        S.hello()
        S.ccs(), S.ccs(), S.ccs()
        S.flight()
        S.app_keys()
        S.client_flight()
        S.request()
        S.data(REPLY)
        S.bye()
        S.expect_bye()
    good('ChangeCipherSpec before the hello and three after', several_ccs)

    good('EncryptedExtensions answering server_name and supported_groups',
         ee=msg(ENCRYPTED_EXTENSIONS, vec16(ext(0, b'') + ext(10, vec16(b'\x00\x1d\x00\x17')))))
    good('the flight in one record', layout='one')
    good('the flight in records of one to seven bytes', layout=[1, 2, 3, 4, 5, 6, 7])
    good('the flight a byte to a record', layout=1, ccs=False)
    good('every record padded', pad=37)

    def tiny(S):
        S.handshake()
        S.request()
        S.data(REPLY, size=1)
        S.bye()
        S.expect_bye()
    good('the reply a byte to a record', tiny)

    def padded(S):
        S.handshake()
        S.request()
        S.data(b'', pad=100)  # nothing but padding: allowed for application data (5.4)
        S.data(REPLY[:10], pad=1)
        S.data(b'')
        S.data(REPLY[10:], size=5, pad=255)
        S.data(b'', pad=16384)  # as much padding as fits
        S.send(ALERT, bytes([1, CLOSE_NOTIFY]), pad=300)
        S.expect_bye()
    good('empty records and padded ones', padded)

    big = bytes((i * 7 + i // 251) & 0xff for i in range(16384))

    def full(S):
        S.handshake()
        S.request()
        S.data(big)
        S.data(big[:1000])
        S.bye()
        S.expect_bye()
    good('a record as large as records get', full, data=big + big[:1000])

    good('a request too long for one record', request=bytes(range(256)) * 64 + b'!')

    good('a request to retry, with a cookie', retry=b'come back with this', abuse=True)
    good('a retry with a cookie of 1000 bytes', retry=bytes(range(250)) * 4)
    good('a retry, and no ChangeCipherSpec', retry=b'c', ccs=False)

    good('the server asks for a certificate', creq=True)
    good('asked for a certificate, flight in one record', creq=True, layout='one')
    good('asked for a certificate after a retry', creq=True, retry=b'cookie')

    def tickets(S):
        S.handshake()
        S.send(HANDSHAKE, S.ticket() + S.ticket())  # two in a record
        S.request()
        S.data(REPLY[:20])
        S.send(HANDSHAKE, S.ticket(), size=9)  # one across many
        S.data(REPLY[20:])
        S.send(HANDSHAKE, S.ticket(), pad=10)
        S.bye()
        S.expect_bye()
    good('session tickets, before the reply and in the middle of it', tickets)

    def updates(S):
        S.handshake()
        S.request()
        S.data(REPLY[:5])
        S.key_update()
        S.data(REPLY[5:25])
        S.key_update()
        S.key_update()
        S.data(REPLY[25:])
        S.bye()
        S.expect_bye()
    good('the server updates its key three times', updates, abuse=True)

    def updates_asked(S):
        S.handshake()
        S.request()
        S.key_update(ask=True)  # the client's answer, and all it sends after, must be under its next key
        S.data(REPLY[:25])
        S.request(S.case.again)
        S.key_update(ask=True)
        S.key_update()
        S.data(REPLY[25:])
        S.bye()
        S.expect_bye()
    good('the server asks the client to update its key, twice', updates_asked, again=b'and another thing\n')

    def update_split(S):
        S.handshake()
        S.request()
        S.send(HANDSHAKE, S.ticket() + msg(KEY_UPDATE, b'\x00'), size=3)  # a ticket, then the update ends a record
        S.wr = S.wr.next()
        S.data(REPLY)
        S.bye()
        S.expect_bye()
    good('a ticket and a key update in three-byte records', update_split)

    # ---- AES-128-GCM, the suite every server must take ----
    good('AES-128-GCM', suite=0x1301, abuse=True)
    good('AES-128-GCM after a retry', suite=0x1301, retry=b'cookie')

    def aes_odd(S):
        S.handshake(suite=0x1301, layout=7)
        S.request()
        S.key_update(ask=True)
        S.data(REPLY[:25], size=3, pad=17)
        S.request(S.case.again)
        S.data(REPLY[25:])
        S.data(big)
        S.bye()
        S.expect_bye()
    good('AES-128-GCM in seven-byte records, padding, a key update, a full record', aes_odd,
         again=b'and another thing\n', data=REPLY + big)

    junk = bytes((i * 13) & 0xff for i in range(20000))
    good('a long chain of certificates',
         certs=[(CERT, ext(5, b'\x01\x00\x00\x00')), junk, (CERT, ext(18, b'') + ext(0x1234, b'abc')), CERT * 3])
    good('ECDSA with P-384 and SHA-384', certs=[CERT384], signer=(KEY384, 0x0503))
    good('RSA-PSS with SHA-256', certs=[RSA_CERT], signer=(RSA_KEY, 0x0804))
    good('RSA-PSS with SHA-384', certs=[RSA_CERT], signer=(RSA_KEY, 0x0805))
    good('RSA-PSS with SHA-512', certs=[RSA_CERT], signer=(RSA_KEY, 0x0806), abuse=True)
    good('an address, so no server name', host='127.0.0.1')
    good('no server name given', host='')
    good('a server name with a dot at the end', host='example.test.')

    # The verifier's answers, and the alert each sends (RFC 8446 4.4.2.4,
    # 4.4.3, 6.2). TLS itself takes no view of what a certificate says:
    # these two get through until a verifier says no.
    bad_flight('a certificate that is not one, which the verifier turns down', E_CERT_BAD, BAD_CERTIFICATE,
               certs=[b'this is not a certificate'], verdict=E_CERT_BAD, sig_good=False)
    bad_flight('a signature that is not one, which the verifier turns down', E_CERT_SIGNATURE, DECRYPT_ERROR,
               cv=msg(CERTIFICATE_VERIFY, b'\x04\x03' + vec16(b'this is not a signature')),
               verdict=E_CERT_SIGNATURE, sig_good=False)
    bad_flight('a signature by some other key', E_CERT_SIGNATURE, DECRYPT_ERROR, signer=(KEY384, 0x0503),
               verdict=E_CERT_SIGNATURE, sig_good=False)
    for err, alert, what in ((E_CERT_BAD, BAD_CERTIFICATE, 'bad'), (E_CERT_UNSUPPORTED, UNSUPPORTED_CERTIFICATE,
                             'unsupported'), (E_CERT_EXPIRED, CERTIFICATE_EXPIRED, 'expired'),
                             (E_CERT_UNKNOWN_CA, UNKNOWN_CA, 'from nobody we trust'),
                             (E_CERT_NAME, BAD_CERTIFICATE, 'for some other name')):
        bad_flight('the verifier says the certificate is ' + what, err, alert, verdict=err)
    bad_flight('the verifier refuses an RSA server', E_CERT_UNKNOWN_CA, UNKNOWN_CA, certs=[RSA_CERT],
               signer=(RSA_KEY, 0x0804), verdict=E_CERT_UNKNOWN_CA)
    bad_flight('the verifier refuses an address', E_CERT_NAME, BAD_CERTIFICATE, host='127.0.0.1', verdict=E_CERT_NAME)
    bad_flight('the verifier refuses, after a certificate request', E_CERT_UNKNOWN_CA, UNKNOWN_CA,
               order=('ee', 'creq', 'cert', 'cv', 'fin'), verdict=E_CERT_UNKNOWN_CA)
    bad_flight('the verifier runs out of memory', E_NOMEM, INTERNAL_ERROR, verdict=E_NOMEM)
    bad_flight('the verifier fails with an errno', -5, INTERNAL_ERROR, verdict=-5)
    bad_flight('the verifier answers something that is not an answer', E_CERT_BAD, BAD_CERTIFICATE, verdict=7)

    # Schemes the server may not sign the handshake with: PKCS#1, which is
    # for certificates only, and those we did not offer. Turned down
    # before any verifier is asked.
    for scheme, what in ((0x0401, 'RSA PKCS#1 with SHA-256 (certificates only)'),
                         (0x0601, 'RSA PKCS#1 with SHA-512 (certificates only)'),
                         (0x0603, 'ECDSA with P-521, not offered'), (0x0807, 'Ed25519, not offered'),
                         (0x0809, 'RSA-PSS with a PSS key, not offered'), (0x0201, 'RSA with SHA-1'),
                         (0x0000, 'scheme 0')):
        key = RSA_KEY if scheme in PKCS1_HASH else KEY
        bad_flight('a signature with ' + what, E_PARAM, ILLEGAL_PARAMETER, certs=[RSA_CERT if key is RSA_KEY else CERT],
                   signer=(key, scheme), sig_good=False)

    def early(S):
        S.handshake(early=REPLY[:9])  # sent before the client's Finished has arrived
        S.request()
        S.data(REPLY[9:])
        S.bye()
        S.expect_bye()
    good('the server starts talking before the client has finished', early)

    def no_reply(S):
        S.handshake()
        S.request()
        S.bye()
        S.expect_bye()
    good('nothing but a goodbye', no_reply, data=b'')

    def canceled(S):
        S.handshake()
        S.request()
        S.data(REPLY)
        S.send(ALERT, bytes([1, USER_CANCELED]))  # to be ignored: the goodbye follows (6.1)
        S.bye()
        S.expect_bye()
    good('user_canceled before the goodbye', canceled)

    def after_bye(S):
        S.handshake()
        S.request()
        S.data(REPLY)
        S.bye()
        S.data(b'this is not to be read')
        S.expect_bye()
    good('data after the goodbye is not read', after_bye)

    # ---- Not TLS at all: no alert, since nobody there would understand one ----
    def not_tls(name, reply, error=E_NOT_TLS):
        bad_first(name, error, None, lambda S, hello: S.raw(reply))
    not_tls('an HTTP server on the port', b'HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nplain HTTP here\n')
    not_tls('an SSH server on the port', b'SSH-2.0-OpenSSH_9.6\r\n')
    not_tls('a record of some version 2', b'\x16\x02\x00\x00\x04abcd')
    not_tls('a record type that does not exist', b'\x18\x03\x03\x00\x04abcd')
    not_tls('a server that says nothing', b'', E_EOF)
    not_tls('a server that hangs up after three bytes', b'\x16\x03\x03', E_EOF)
    case('a name that no certificate could be for', lambda S: S.silent(), connect=E_NAME, host='not a host name!')
    case('a name too long to be one', lambda S: S.silent(), connect=E_NAME, host='a.' * 127 + 'a')
    good('a name as long as names get', host='a.' * 126 + 'b')

    # ---- A ServerHello gone wrong (alerts in the clear) ----
    bad_hello('a TLS 1.2 server: a hello with no extensions', E_VERSION, PROTOCOL_VERSION,
              bare=True, sid=bytes(range(32)), suite=0xc02f)
    bad_hello('a TLS 1.2 server: extensions, none of them supported_versions', E_VERSION, PROTOCOL_VERSION,
              exts=ext(0xff01, b'\x00') + ext(11, b'\x01\x00'), suite=0xcca8)
    bad_hello('supported_versions chooses TLS 1.2', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: ext(43, b'\x03\x03') + S.share())
    bad_hello('supported_versions chooses a version from the future', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: ext(43, b'\x03\x05') + S.share())
    bad_hello('legacy_version is not 0x0303', E_PARAM, ILLEGAL_PARAMETER, version=0x0304)
    bad_hello('a cipher suite that was not offered', E_PARAM, ILLEGAL_PARAMETER, suite=0x1302)
    bad_hello('a suite other than the one the retry named', E_PARAM, ILLEGAL_PARAMETER, retry=b'c', suite=0x1301,
              retry_suite=0x1303)
    bad_hello('the other way round', E_PARAM, ILLEGAL_PARAMETER, retry=b'c', suite=0x1303, retry_suite=0x1301)
    bad_hello('someone else\'s session id', E_PARAM, ILLEGAL_PARAMETER, sid=bytes(range(32)))
    bad_hello('no session id', E_PARAM, ILLEGAL_PARAMETER, sid=b'')
    bad_hello('compression', E_PARAM, ILLEGAL_PARAMETER, comp=1)
    bad_hello('a key share for a group that was not offered', E_PARAM, ILLEGAL_PARAMETER,
              exts=SV + ext(51, struct.pack('>H', 0x17) + vec16(b'\x04' + bytes(range(64)))))
    bad_hello('an x25519 share of 31 bytes', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: SV + S.share(S.public[:31]))
    bad_hello('an x25519 share of 33 bytes', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: SV + S.share(S.public + b'\x00'))
    bad_hello('a share of all zeros', E_PARAM, ILLEGAL_PARAMETER, exts=lambda S: SV + S.share(bytes(32)))
    bad_hello('a share that is the number one', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: SV + S.share(b'\x01' + bytes(31)))
    bad_hello('a share of small order', E_PARAM, ILLEGAL_PARAMETER,
              exts=lambda S: SV + S.share(bytes.fromhex(
                  'e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800')))
    bad_hello('no key share', E_PARAM, MISSING_EXTENSION, exts=SV)
    bad_hello('an extension nobody asked for', E_PARAM, UNSUPPORTED_EXTENSION,
              exts=lambda S: SV + S.share() + ALPN)
    bad_hello('a cookie in a ServerHello', E_PARAM, UNSUPPORTED_EXTENSION,
              exts=lambda S: SV + S.share() + ext(44, vec16(b'cookie')))
    bad_hello('supported_versions twice', E_PARAM, ILLEGAL_PARAMETER, exts=lambda S: SV + S.share() + SV)
    bad_hello('key_share twice', E_PARAM, ILLEGAL_PARAMETER, exts=lambda S: SV + S.share() + S.share())
    bad_hello('bytes after the extensions', E_DECODE, DECODE_ERROR, tail=b'\x00')
    bad_hello('supported_versions a byte too long', E_DECODE, DECODE_ERROR,
              exts=lambda S: ext(43, b'\x03\x04\x00') + S.share())
    bad_hello('a key share shorter than it says', E_DECODE, DECODE_ERROR,
              exts=lambda S: SV + ext(51, struct.pack('>HH', 0x1d, 32) + S.public[:20]))
    bad_hello('an extension that runs past the end', E_DECODE, DECODE_ERROR,
              exts=SV + struct.pack('>HH', 51, 200) + b'\x00\x1d')
    bad_first('a hello cut off in the random field', E_DECODE, DECODE_ERROR,
              lambda S, hello: S.plain(HANDSHAKE, msg(SERVER_HELLO, b'\x03\x03' + bytes(20))))
    bad_first('a hello and something else in one record', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S, hello: S.plain(HANDSHAKE, S.make_hello(hello) + msg(ENCRYPTED_EXTENSIONS, b'\x00\x00')))
    bad_first('EncryptedExtensions where the hello should be', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S, hello: S.plain(HANDSHAKE, msg(ENCRYPTED_EXTENSIONS, b'\x00\x00')))
    bad_first('application data where the hello should be', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S, hello: S.plain(APPDATA, b'hello?'))
    bad_first('an empty handshake record', E_PROTOCOL, UNEXPECTED_MESSAGE, lambda S, hello: S.plain(HANDSHAKE, b''))
    bad_first('a record longer than records may be', E_TOO_BIG, RECORD_OVERFLOW,
              lambda S, hello: S.raw(struct.pack('>BHH', HANDSHAKE, 0x0303, 16385) + bytes(100)))
    bad_first('a message longer than we will take', E_TOO_BIG, ILLEGAL_PARAMETER,
              lambda S, hello: S.plain(HANDSHAKE, bytes([SERVER_HELLO]) + (128 * 1024 + 1).to_bytes(3, 'big')))
    bad_first('ChangeCipherSpec with the wrong byte in it', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S, hello: S.ccs(b'\x02'))
    bad_first('ChangeCipherSpec of two bytes', E_PROTOCOL, UNEXPECTED_MESSAGE, lambda S, hello: S.ccs(b'\x01\x01'))
    bad_first('ChangeCipherSpec of no bytes', E_PROTOCOL, UNEXPECTED_MESSAGE, lambda S, hello: S.ccs(b''))
    bad_first('the server refuses: handshake_failure', E_ALERT - HANDSHAKE_FAILURE, None,
              lambda S, hello: S.plain(ALERT, bytes([2, HANDSHAKE_FAILURE])))
    bad_first('the server refuses: protocol_version', E_ALERT - PROTOCOL_VERSION, None,
              lambda S, hello: S.plain(ALERT, bytes([2, PROTOCOL_VERSION])))
    bad_first('an alert marked as a warning is fatal all the same', E_ALERT - UNRECOGNIZED_NAME, None,
              lambda S, hello: S.plain(ALERT, bytes([1, UNRECOGNIZED_NAME])))
    bad_first('an alert nobody has heard of', E_ALERT - 255, None, lambda S, hello: S.plain(ALERT, bytes([1, 255])))
    bad_first('an alert of one byte', E_DECODE, DECODE_ERROR, lambda S, hello: S.plain(ALERT, b'\x02'))
    bad_first('an alert of three bytes', E_DECODE, DECODE_ERROR, lambda S, hello: S.plain(ALERT, b'\x02\x28\x00'))
    bad_first('a goodbye where the hello should be', E_EOF, None,
              lambda S, hello: S.plain(ALERT, bytes([1, CLOSE_NOTIFY])))
    bad_first('half a hello, then nothing', E_EOF, None,
              lambda S, hello: S.raw((struct.pack('>BHH', HANDSHAKE, 0x0303, 90) + S.make_hello(hello))[:50]))

    # A request to retry gone wrong.
    bad_first('a retry that asks for nothing new', E_PARAM, ILLEGAL_PARAMETER,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=SV)))
    bad_first('a retry asking for the share it already has', E_PARAM, ILLEGAL_PARAMETER,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=SV + ext(51, b'\x00\x1d') +
                                                               ext(44, vec16(b'cookie')))))
    bad_first('a retry asking for a group that was not offered', E_PARAM, ILLEGAL_PARAMETER,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=SV + ext(51, b'\x00\x17'))))
    bad_first('a retry with an empty cookie', E_DECODE, DECODE_ERROR,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=SV + ext(44, vec16(b'')))))
    bad_first('a retry with two cookies', E_PARAM, ILLEGAL_PARAMETER,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=SV + ext(44, vec16(b'a')) +
                                                               ext(44, vec16(b'b')))))
    bad_first('a retry with no supported_versions', E_VERSION, PROTOCOL_VERSION,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello, exts=ext(44, vec16(b'cookie')))))
    bad_first('a retry and something else in one record', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S, hello: S.plain(HANDSHAKE, S.make_retry(hello) + b'\x02\x00'))

    def retry_twice(S):
        hello = S.client_hello()
        S.transcript = hello['whole']
        hello = S.retry(hello, b'first')
        S.plain(HANDSHAKE, S.make_retry(hello, b'second'))
        S.alert(UNEXPECTED_MESSAGE)
    case('a second request to retry', retry_twice, connect=E_PROTOCOL)

    # ---- The sealed flight gone wrong (alerts under the handshake key) ----
    def ee(exts):
        return msg(ENCRYPTED_EXTENSIONS, vec16(exts))
    bad_flight('ALPN, which was not asked for', E_PARAM, UNSUPPORTED_EXTENSION, ee=ee(ALPN))
    bad_flight('an extension from the private range', E_PARAM, UNSUPPORTED_EXTENSION, ee=ee(ext(0xff01, b'\x00')))
    bad_flight('key_share in EncryptedExtensions', E_PARAM, UNSUPPORTED_EXTENSION,
               ee=ee(ext(51, b'\x00\x1d\x00\x00')))
    bad_flight('server_name twice', E_PARAM, ILLEGAL_PARAMETER, ee=ee(ext(0, b'') + ext(0, b'')))
    bad_flight('EncryptedExtensions with a byte after the list', E_DECODE, DECODE_ERROR,
               ee=msg(ENCRYPTED_EXTENSIONS, vec16(b'') + b'\x00'))
    bad_flight('EncryptedExtensions with no list at all', E_DECODE, DECODE_ERROR, ee=msg(ENCRYPTED_EXTENSIONS, b''))
    bad_flight('an extension that runs past the end of the list', E_DECODE, DECODE_ERROR,
               ee=ee(struct.pack('>HH', 0, 5) + b'ab'))

    bad_flight('no EncryptedExtensions', E_PROTOCOL, UNEXPECTED_MESSAGE, order=('cert', 'cv', 'fin'))
    bad_flight('no Certificate', E_PROTOCOL, UNEXPECTED_MESSAGE, order=('ee', 'cv', 'fin'))
    bad_flight('no CertificateVerify', E_PROTOCOL, UNEXPECTED_MESSAGE, order=('ee', 'cert', 'fin'))
    bad_flight('nothing between EncryptedExtensions and Finished', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'fin'))
    bad_flight('CertificateVerify before Certificate', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'cv', 'cert', 'fin'))
    bad_flight('CertificateRequest twice', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'creq', 'creq', 'cert', 'cv', 'fin'))
    bad_flight('CertificateRequest after Certificate', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'cert', 'creq', 'cv', 'fin'))
    bad_flight('EncryptedExtensions twice', E_PROTOCOL, UNEXPECTED_MESSAGE, order=('ee', 'ee', 'cert', 'cv', 'fin'))
    bad_flight('a ServerHello again', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'sh', 'cert', 'cv', 'fin'), sh=msg(SERVER_HELLO, bytes(40)))
    bad_flight('a key update during the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'ku', 'cert', 'cv', 'fin'), ku=msg(KEY_UPDATE, b'\x00'))
    bad_flight('a session ticket during the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'cert', 'cv', 'nst', 'fin'), nst=msg(NEW_SESSION_TICKET, bytes(20)))
    bad_flight('a message type that does not exist', E_PROTOCOL, UNEXPECTED_MESSAGE,
               order=('ee', 'odd', 'cert', 'cv', 'fin'), odd=msg(99, b'what'))

    def cert_raw(body):
        return msg(CERTIFICATE, body)
    bad_flight('an empty list of certificates', E_DECODE, DECODE_ERROR, cert=cert_raw(vec8(b'') + vec24(b'')))
    bad_flight('a certificate of no bytes', E_DECODE, DECODE_ERROR,
               cert=cert_raw(vec8(b'') + vec24(vec24(b'') + vec16(b''))))
    bad_flight('a certificate request context where there is no request', E_PARAM, ILLEGAL_PARAMETER,
               cert=cert_raw(vec8(b'ctx') + vec24(vec24(CERT) + vec16(b''))))
    bad_flight('a certificate longer than its list', E_DECODE, DECODE_ERROR,
               cert=cert_raw(vec8(b'') + vec24((len(CERT) + 50).to_bytes(3, 'big') + CERT + vec16(b''))))
    bad_flight('a certificate with no extension list', E_DECODE, DECODE_ERROR,
               cert=cert_raw(vec8(b'') + vec24(vec24(CERT))))
    bad_flight('a byte after the list of certificates', E_DECODE, DECODE_ERROR,
               cert=cert_raw(vec8(b'') + vec24(vec24(CERT) + vec16(b'')) + b'\x00'))
    bad_flight('a Certificate message of no bytes', E_DECODE, DECODE_ERROR, cert=cert_raw(b''))
    bad_flight('an empty signature', E_DECODE, DECODE_ERROR, cv=msg(CERTIFICATE_VERIFY, b'\x04\x03' + vec16(b'')))
    bad_flight('a byte after the signature', E_DECODE, DECODE_ERROR,
               cv=msg(CERTIFICATE_VERIFY, b'\x04\x03' + vec16(b'sig') + b'\x00'))
    bad_flight('a signature shorter than it says', E_DECODE, DECODE_ERROR,
               cv=msg(CERTIFICATE_VERIFY, b'\x04\x03\x00\x40abc'))
    creq_order = ('ee', 'creq', 'cert', 'cv', 'fin')
    bad_flight('a certificate request with a context', E_PARAM, ILLEGAL_PARAMETER, order=creq_order,
               creq=msg(CERTIFICATE_REQUEST, vec8(b'x') + vec16(ext(13, vec16(b'\x04\x03')))))
    bad_flight('a certificate request with no signature_algorithms', E_PARAM, MISSING_EXTENSION, order=creq_order,
               creq=msg(CERTIFICATE_REQUEST, vec8(b'') + vec16(ext(47, vec16(b'')))))
    bad_flight('a certificate request with signature_algorithms twice', E_PARAM, ILLEGAL_PARAMETER,
               order=creq_order, creq=msg(CERTIFICATE_REQUEST, vec8(b'') + vec16(ext(13, vec16(b'\x04\x03')) * 2)))
    bad_flight('a certificate request cut short', E_DECODE, DECODE_ERROR, order=creq_order,
               creq=msg(CERTIFICATE_REQUEST, vec8(b'') + b'\x00\x09\x00'))

    def wrong_finished(S):
        mac = S.finished_mac(S.wr.secret)
        return msg(FINISHED, mac[:31] + bytes([mac[31] ^ 1]))
    for name, fin, error, alert in (
            ('a Finished wrong in its last bit', wrong_finished, E_FINISHED, DECRYPT_ERROR),
            ('a Finished of all zeros', lambda S: msg(FINISHED, bytes(32)), E_FINISHED, DECRYPT_ERROR),
            ('a Finished of 31 bytes', lambda S: msg(FINISHED, S.finished_mac(S.wr.secret)[:31]), E_DECODE,
             DECODE_ERROR),
            ('a Finished of 33 bytes', lambda S: msg(FINISHED, S.finished_mac(S.wr.secret) + b'\x00'), E_DECODE,
             DECODE_ERROR),
            ('a Finished of 48 bytes, as SHA-384 would make', lambda S: msg(FINISHED, bytes(48)), E_DECODE,
             DECODE_ERROR)):
        def send(S, fin=fin):
            S.flight(fin=lambda: fin(S))
        bad_flight(name, error, alert, send)

    def finished_and_more(S):
        S.flight(order=('ee', 'cert', 'cv', 'fin', 'nst'), layout='one', nst=S.ticket())
    bad_flight('Finished with a ticket behind it in the record', E_PROTOCOL, UNEXPECTED_MESSAGE, finished_and_more)

    def app_data_early(S):
        S.flight(order=('ee', 'cert', 'cv'))
        S.data(b'too soon')
    bad_flight('application data before Finished', E_PROTOCOL, UNEXPECTED_MESSAGE, app_data_early)

    def app_data_inside(S):
        S.transcript += S.m_ee()
        whole = S.m_cert()
        S.send(HANDSHAKE, S.m_ee() + whole[:100])
        S.send(ALERT, bytes([1, USER_CANCELED]))  # an alert may not come in the middle of a message either,
        S.send(HANDSHAKE, whole[100:])            # but this client lets that one through; data it does not
        S.data(b'in the middle')
    bad_flight('application data between two messages', E_PROTOCOL, UNEXPECTED_MESSAGE, app_data_inside)

    # Records gone wrong.
    def record(**kw):
        def send(S):
            m = S.m_ee()
            S.sealed(HANDSHAKE, m, **kw)
            S.flight(order=('cert', 'cv', 'fin'))
        return send
    bad_flight('a record with its tag wrong', E_BAD_MAC, BAD_RECORD_MAC, record(damage=flip_last))
    bad_flight('a record with its first byte wrong', E_BAD_MAC, BAD_RECORD_MAC, record(damage=flip_first))
    bad_flight('a record cut down to its tag', E_BAD_MAC, BAD_RECORD_MAC, record(damage=lambda b: b[-16:]))
    bad_flight('AES-128-GCM, a record with its tag wrong', E_BAD_MAC, BAD_RECORD_MAC, record(damage=flip_last),
               suite=0x1301)
    bad_flight('AES-128-GCM, a record with its first byte wrong', E_BAD_MAC, BAD_RECORD_MAC,
               record(damage=flip_first), suite=0x1301)
    bad_flight('a sealed record that says it is a handshake record', E_PROTOCOL, UNEXPECTED_MESSAGE,
               record(outer=HANDSHAKE))
    bad_flight('a sealed record of nothing but zeros', E_PROTOCOL, UNEXPECTED_MESSAGE, record(inner=bytes(20)))
    bad_flight('a sealed record of nothing', E_PROTOCOL, UNEXPECTED_MESSAGE, record(inner=b''))
    bad_flight('a ChangeCipherSpec inside a sealed record', E_PROTOCOL, UNEXPECTED_MESSAGE,
               record(inner=b'\x01' + bytes([CCS])))
    bad_flight('a sealed record of a type that does not exist', E_PROTOCOL, UNEXPECTED_MESSAGE,
               record(inner=b'abc' + bytes([24])))
    bad_flight('a sealed handshake record with nothing in it', E_PROTOCOL, UNEXPECTED_MESSAGE,
               record(inner=bytes([HANDSHAKE])))
    bad_flight('more than 16385 bytes inside a sealed record', E_TOO_BIG, RECORD_OVERFLOW,
               record(inner=bytes([HANDSHAKE]) + bytes(16385)))
    bad_flight('a sealed record longer than they may be', E_TOO_BIG, RECORD_OVERFLOW,
               lambda S: S.raw(struct.pack('>BHH', APPDATA, 0x0303, 16384 + 257) + bytes(64)))
    bad_flight('a sealed record too short to have a tag', E_BAD_MAC, BAD_RECORD_MAC,
               lambda S: S.raw(struct.pack('>BHH', APPDATA, 0x0303, 15) + bytes(15)))
    bad_flight('a sealed record of no bytes at all', E_BAD_MAC, BAD_RECORD_MAC,
               lambda S: S.raw(struct.pack('>BHH', APPDATA, 0x0303, 0)))
    bad_flight('the flight in the clear', E_PROTOCOL, UNEXPECTED_MESSAGE,
               lambda S: S.plain(HANDSHAKE, S.m_ee()))
    bad_flight('an alert in the clear once there are keys', E_PROTOCOL, UNEXPECTED_MESSAGE,
               lambda S: S.plain(ALERT, bytes([2, HANDSHAKE_FAILURE])))

    def swapped(S):
        S.wr.seq = 1
        S.sealed(HANDSHAKE, S.m_ee())  # the second record first
    bad_flight('records out of order', E_BAD_MAC, BAD_RECORD_MAC, swapped)

    def too_soon(S):
        S.sealed(HANDSHAKE, S.m_ee(), keys=Keys(bytes(32)))
    bad_flight('a record under some other key', E_BAD_MAC, BAD_RECORD_MAC, too_soon)

    def sealed_alert(S):
        S.flight(order=('ee',))
        S.send(ALERT, bytes([2, INTERNAL_ERROR]))
    bad_flight('the server gives up in the middle: internal_error', E_ALERT - INTERNAL_ERROR, None, sealed_alert)

    def sealed_bye(S):
        S.flight(order=('ee', 'cert'))
        S.bye()
    bad_flight('a goodbye in the middle of the handshake', E_EOF, None, sealed_bye)

    def hang_up(S):
        S.flight(order=('ee', 'cert'))
    bad_flight('the server hangs up in the middle of its flight', E_EOF, None, hang_up)

    def late_ccs(S):
        S.flight()
        S.ccs()  # the handshake is over for the server: no more of these (section 5)
        S.app_keys()
        S.bye()
        S.client_flight()
        S.request()
        S.alert(UNEXPECTED_MESSAGE)
    case('ChangeCipherSpec after the server\'s Finished', lambda S: (S.hello(), late_ccs(S)), final=E_PROTOCOL)

    # ---- After the handshake (alerts under the application key) ----
    bad_after('a key update with a value that means nothing', E_PARAM, ILLEGAL_PARAMETER,
              lambda S: S.send(HANDSHAKE, msg(KEY_UPDATE, b'\x02')))
    bad_after('a key update with no value', E_DECODE, DECODE_ERROR,
              lambda S: S.send(HANDSHAKE, msg(KEY_UPDATE, b'')))
    bad_after('a key update with two values', E_DECODE, DECODE_ERROR,
              lambda S: S.send(HANDSHAKE, msg(KEY_UPDATE, b'\x00\x00')))
    bad_after('a key update with a ticket behind it in the record', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.send(HANDSHAKE, msg(KEY_UPDATE, b'\x00') + S.ticket()))

    def stale_key(S):
        S.data(REPLY[:10])
        S.send(HANDSHAKE, msg(KEY_UPDATE, b'\x00'))
        S.data(REPLY[10:])  # still under the old key
    bad_after('a key update, and then the old key', E_BAD_MAC, BAD_RECORD_MAC, stale_key, data=REPLY[:10])

    def early_key(S):
        S.data(REPLY[:10])
        S.wr = S.wr.next()
        S.data(REPLY[10:])  # the next key, with no update announced
    bad_after('the next key, with no key update', E_BAD_MAC, BAD_RECORD_MAC, early_key, data=REPLY[:10])

    bad_after('a certificate request after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.send(HANDSHAKE, S.m_creq(context=b'later')))
    bad_after('a ServerHello after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.send(HANDSHAKE, msg(SERVER_HELLO, bytes(40))))
    bad_after('a Finished after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.send(HANDSHAKE, msg(FINISHED, bytes(32))))
    bad_after('a HelloRequest, as TLS 1.2 had', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.send(HANDSHAKE, msg(0, b'')))
    bad_after('an empty handshake record after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.sealed(HANDSHAKE, b''))
    bad_after('a message longer than we will take, after the handshake', E_TOO_BIG, ILLEGAL_PARAMETER,
              lambda S: S.send(HANDSHAKE, bytes([NEW_SESSION_TICKET]) + (128 * 1024 + 1).to_bytes(3, 'big')))

    def data_in_ticket(S):
        t = S.ticket()
        S.send(HANDSHAKE, t[:30])
        S.data(REPLY)
        S.send(HANDSHAKE, t[30:])
    bad_after('data in the middle of a ticket', E_PROTOCOL, UNEXPECTED_MESSAGE, data_in_ticket)

    def bye_in_ticket(S):
        S.data(REPLY)
        S.send(HANDSHAKE, S.ticket()[:30])
        S.bye()
    bad_after('a goodbye in the middle of a ticket', E_PROTOCOL, UNEXPECTED_MESSAGE, bye_in_ticket, data=REPLY)

    def damaged_data(S):
        S.data(REPLY[:20])
        S.sealed(APPDATA, REPLY[20:], damage=lambda b: bytes([b[0] ^ 1]) + b[1:])
    bad_after('the second record of the reply damaged', E_BAD_MAC, BAD_RECORD_MAC, damaged_data, data=REPLY[:20])

    def replayed(S):
        S.data(REPLY[:20])
        S.raw(bytes(S.sent[-(5 + 20 + 1 + 16):]))  # the same record again
    bad_after('a record sent twice', E_BAD_MAC, BAD_RECORD_MAC, replayed, data=REPLY[:20])

    def dropped(S):
        S.data(REPLY[:20])
        S.wr.seq += 1  # a record that never arrives
        S.data(REPLY[20:])
    bad_after('a record missing', E_BAD_MAC, BAD_RECORD_MAC, dropped, data=REPLY[:20])

    def cut_short(S):
        S.handshake()
        S.request()
        S.data(REPLY[:20])
        header = struct.pack('>BHH', APPDATA, 0x0303, len(REPLY) - 20 + 17)
        S.raw(header + S.wr.seal(header, REPLY[20:] + bytes([APPDATA]))[:7])
        S.silent()
    case('the server hangs up in the middle of a record', cut_short, final=E_EOF, data=REPLY[:20])

    def no_bye(S):
        S.handshake()
        S.request()
        S.data(REPLY)
        S.silent()
    case('the server hangs up without a goodbye', no_bye, final=E_EOF, data=REPLY)

    def no_reply_at_all(S):
        S.handshake()
        S.request()
        S.silent()
    case('the server hangs up when the request has come', no_reply_at_all, final=E_EOF)

    bad_after('the server gives up after the request: internal_error', E_ALERT - INTERNAL_ERROR, None,
              lambda S: (S.data(REPLY[:7]), S.send(ALERT, bytes([2, INTERNAL_ERROR]))), data=REPLY[:7])
    bad_after('an alert of one byte, sealed', E_DECODE, DECODE_ERROR, lambda S: S.send(ALERT, b'\x01'))
    bad_after('an alert of no bytes, sealed', E_DECODE, DECODE_ERROR, lambda S: S.sealed(ALERT, b''))
    bad_after('a record in the clear after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE,
              lambda S: S.plain(HANDSHAKE, S.ticket()))
    bad_after('data in the clear after the handshake', E_BAD_MAC, BAD_RECORD_MAC,
              lambda S: S.plain(APPDATA, b'this was never sealed, tag and all'))
    bad_after('ChangeCipherSpec after the handshake', E_PROTOCOL, UNEXPECTED_MESSAGE, lambda S: S.ccs())
    bad_after('a data record with too much in it', E_TOO_BIG, RECORD_OVERFLOW,
              lambda S: S.sealed(APPDATA, b'', inner=bytes(16385) + bytes([APPDATA])))
    bad_after('a data record longer than they may be', E_TOO_BIG, RECORD_OVERFLOW,
              lambda S: S.raw(struct.pack('>BHH', APPDATA, 0x0303, 16384 + 257)))


# ---- Running a case ----

def build_client(workdir):
    """Our client, for this machine, as test_tls --live."""
    exe = os.path.join(workdir, 'tls_client')
    src = [os.path.join(ROOT, p) for p in (
        'tests/test_tls.c', 'src/programs/lib/tls.c', 'src/programs/lib/chachapoly.c', 'src/programs/lib/x25519.c',
        'src/programs/lib/aesgcm.c', 'src/lib/sha256.c', 'src/lib/chacha20.c')]
    subprocess.run(['gcc', '-m32', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-DNO_CASES',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-Wl,--wrap=x25519', '-Wl,--wrap=x25519_public',  # see test_tls.c
                    '-I' + os.path.join(ROOT, 'src/include'), '-I' + os.path.join(ROOT, 'src/programs/lib'),
                    '-o', exe] + src, check=True)
    return exe


class Result:
    pass


def run_client(exe, c, port):
    return subprocess.Popen([exe, '--live', str(port), str(c.seed), c.host, c.request.hex(), c.again.hex(),
                             str(c.verdict)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def parse_peer(b):
    """What the stand-in verifier was handed, as test_tls.c's note_peer writes it out."""
    at = b.index(0)
    name, at = b[:at].decode(), at + 1

    def take(n):
        nonlocal at
        need(at + n <= len(b), 'what the verifier was handed is cut short')
        at += n
        return b[at - n:at]

    def vec():
        return take(int.from_bytes(take(4), 'big'))
    certs = [vec() for _ in range(int.from_bytes(take(4), 'big'))]
    scheme = int.from_bytes(take(2), 'big')
    signed, sig = vec(), vec()
    need(at == len(b), 'more than the verifier was handed')
    return name, certs, scheme, signed, sig


def client_result(proc, c, certs=None, before_cv=None, cv=None):
    """What the client came to, checked against the case. certs, before_cv, cv: what the server
    sent as its certificates and CertificateVerify, and the transcript hash between them, when known."""
    out, err = proc.communicate(timeout=30)
    need(proc.returncode == 0, 'the client died: ' + err.decode(errors='replace')[-2000:])
    fields = dict(f.split('=') for f in out.decode().split())
    r = Result()
    r.connect, r.final, r.data = int(fields['connect']), int(fields['final']), bytes.fromhex(fields['data'])
    r.verified, r.asked, r.peer = int(fields['verified']), int(fields['asked']), bytes.fromhex(fields['peer'])
    need(r.connect == c.connect, 'tls_connect gave %d, not %d' % (r.connect, c.connect))
    need(r.final == c.final, 'the last read gave %d, not %d' % (r.final, c.final))
    need(r.data == c.data, 'the client read %d bytes, not the %d it should have' % (len(r.data), len(c.data)))

    # The verifier: asked once at most, always before a connection is made, and believed.
    need(r.asked in (0, 1), 'the verifier was asked %d times' % r.asked)
    need(r.connect != 0 or r.asked, 'connected without asking the verifier')
    need(c.verdict == 0 or r.asked, 'the case has the verifier refuse, but it was never asked')
    need(r.verified == (r.connect == 0 and c.verdict == 0), 'tls_peer_verified says %d' % r.verified)
    if r.asked:
        name, got_certs, scheme, signed, sig = parse_peer(r.peer)
        need(name == (c.sni() or c.host), 'the verifier was told the name was %r' % name)
        need(got_certs and len(signed) == len(CV_CONTEXT) + 32 and signed.startswith(CV_CONTEXT),
             'the verifier was not handed a certificate and what TLS 1.3 says is signed')
        if certs is not None:
            need(got_certs == certs, 'the verifier was handed other certificates than the server sent')
        if before_cv is not None:
            need(signed == CV_CONTEXT + before_cv, 'the client\'s transcript is not the server\'s')
        if cv is not None:
            need(cv[4:6] == struct.pack('>H', scheme) and cv[8:] == sig, 'not the signature the server sent')
        # And the real test of the client's transcript: whatever signed it, the signature verifies.
        good = verify(got_certs[0], scheme, signed, sig)
        need(good == c.sig_good, 'the server\'s signature %s, over what the client says it signed'
             % ('does not verify' if c.sig_good else 'verifies'))
    return r


def record_scripted(exe, c):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    proc = run_client(exe, c, listener.getsockname()[1])
    listener.settimeout(10)
    sock, _ = listener.accept()
    listener.close()
    sock.settimeout(10)
    S = Server(sock, c)
    try:
        c.run(S)
        S.finish()
    except socket.timeout:
        proc.kill()
        raise Wrong('the client and the server are each waiting for the other')
    finally:
        sock.close()
    c.result = client_result(proc, c, S.sent_certs, S.before_cv, S.cv)
    return bytes(S.sent), bytes(S.got)


def pem_files(workdir, key=KEY, cert_der=CERT):
    cert, keyfile = os.path.join(workdir, 'cert.pem'), os.path.join(workdir, 'key.pem')
    with open(cert, 'wb') as f:
        f.write(x509.load_der_x509_certificate(cert_der).public_bytes(serialization.Encoding.PEM))
    with open(keyfile, 'wb') as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    return cert, keyfile


def record_python_ssl(exe, c, workdir, reply, key=KEY, cert_der=CERT):
    """OpenSSL, as Python's ssl module drives it."""
    cert, keyfile = pem_files(workdir, key, cert_der)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(cert, keyfile)
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    proc = run_client(exe, c, listener.getsockname()[1])
    listener.settimeout(10)
    sock, _ = listener.accept()
    listener.close()
    sock.settimeout(10)
    incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
    tls = ctx.wrap_bio(incoming, outgoing, server_side=True)
    sent, got = bytearray(), bytearray()

    def flush():
        data = outgoing.read()
        if data:
            sent.extend(data)
            sock.sendall(data)

    def step(what):
        while True:
            try:
                r = what()
                flush()
                return r
            except ssl.SSLWantReadError:
                flush()
                data = sock.recv(65536)
                got.extend(data)
                if data:
                    incoming.write(data)
                else:
                    incoming.write_eof()

    step(tls.do_handshake)
    need(tls.version() == 'TLSv1.3' and tls.cipher()[0] == 'TLS_CHACHA20_POLY1305_SHA256', 'not what was offered')
    request = b''
    while len(request) < len(c.request):
        request += step(lambda: tls.read(65536))
    need(request == c.request, 'OpenSSL did not read the request we sent')
    at = 0
    for n in reply:
        step(lambda: tls.write(c.data[at:at + n]))
        at += n
    step(tls.unwrap)  # its goodbye, and ours must come back
    sock.shutdown(socket.SHUT_WR)
    while True:
        data = sock.recv(65536)
        if not data:
            break
        got.extend(data)
    sock.close()
    c.result = client_result(proc, c, [cert_der])
    return bytes(sent), bytes(got)


def record_s_server(exe, c, workdir, options, typed, must_print):
    """OpenSSL's s_server program, through a go-between that writes down what passes.
    typed: what to type at it once the client is connected, with a pause before each line."""
    cert, key = pem_files(workdir)
    probe = socket.socket()
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
    probe.close()
    server = subprocess.Popen(['openssl', 's_server', '-accept', '127.0.0.1:%d' % port, '-cert', cert, '-key', key,
                               '-tls1_3', '-naccept', '1'] + options,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    printed = []

    def collect():
        for line in server.stdout:
            printed.append(line)
    threading.Thread(target=collect, daemon=True).start()
    upstream = None
    for _ in range(100):
        try:
            upstream = socket.create_connection(('127.0.0.1', port), timeout=10)
            break
        except OSError:
            time.sleep(0.05)
    need(upstream is not None, 's_server did not start: ' + b''.join(printed).decode(errors='replace'))

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    proc = run_client(exe, c, listener.getsockname()[1])
    listener.settimeout(10)
    sock, _ = listener.accept()
    listener.close()
    sent, got = bytearray(), bytearray()

    def carry(src, dst, log):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                log.extend(data)
                dst.sendall(data)
        except OSError:
            pass
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass
    threads = [threading.Thread(target=carry, args=(sock, upstream, got)),
               threading.Thread(target=carry, args=(upstream, sock, sent))]
    for t in threads:
        t.start()
    for pause, line in typed:
        time.sleep(pause)
        try:
            server.stdin.write(line)
            server.stdin.flush()
        except OSError:
            break
    for t in threads:
        t.join(15)
    try:
        server.wait(5)
    except subprocess.TimeoutExpired:
        server.kill()
    sock.close()
    upstream.close()
    time.sleep(0.1)
    text = b''.join(printed)
    for must in must_print:
        need(must in text, 's_server never printed %r; it printed:\n%s' % (must, text.decode(errors='replace')))
    need(b'error' not in text.lower(), 's_server complained:\n' + text.decode(errors='replace'))
    c.result = client_result(proc, c, [CERT])
    return bytes(sent), bytes(got)


def ignorable_bytes(server):
    """How many bytes of a server's stream TLS says to ignore: the two version bytes in the header
    of each record that is not sealed, but for the first of all, which is how a TLS server is known."""
    at = plain = 0
    while at + 5 <= len(server):
        if server[at] != APPDATA:
            plain += 1
        at += 5 + int.from_bytes(server[at + 3:at + 5], 'big')
    return max(2 * plain - 1, 0)


def hex_lines(b):
    if not b:
        return '""'
    h = b.hex()
    return '\n'.join('      "%s"' % h[i:i + 120] for i in range(0, len(h), 120))


def main():
    check_against_rfc8448()
    define_cases()
    out = []
    with tempfile.TemporaryDirectory() as workdir:
        exe = build_client(workdir)
        failed = 0

        def record(c, how):
            nonlocal failed
            try:
                server, client = how()
            except Wrong as e:
                failed += 1
                print('WRONG: %s: %s' % (c.name, e))
                return
            out.append((c, server, client))

        for c in CASES:
            record(c, lambda: record_scripted(exe, c))

        # OpenSSL. (Its replies are in pieces so there is more than one record of data.)
        text = b'HTTP/1.0 200 OK\r\n\r\n' + b''.join(b'line %d of the reply from OpenSSL\n' % i for i in range(40))
        c = Case('OpenSSL: a handshake, a request and a reply', None, data=text, abuse=True)
        record(c, lambda: record_python_ssl(exe, c, workdir, [19, 500, len(text)]))
        c = Case('OpenSSL: a reply of 17000 bytes', None, data=bytes(range(250)) * 68)
        record(c, lambda: record_python_ssl(exe, c, workdir, [len(c.data)]))
        c = Case('OpenSSL: an address, so no server name', None, data=b'ok\n', host='127.0.0.1')
        record(c, lambda: record_python_ssl(exe, c, workdir, [3]))
        # Other keys: OpenSSL picks the scheme, from what we offer.
        c = Case('OpenSSL: an RSA key, so RSA-PSS', None, data=b'signed with RSA-PSS\n')
        record(c, lambda: record_python_ssl(exe, c, workdir, [len(c.data)], RSA_KEY, RSA_CERT))
        c = Case('OpenSSL: a P-384 key', None, data=b'signed with P-384\n')
        record(c, lambda: record_python_ssl(exe, c, workdir, [len(c.data)], KEY384, CERT384))

        # s_server sends what is typed at it; a line that starts with K
        # or k is not sent but makes it update its key (K: and ask the
        # client to as well). It hangs up without a goodbye when told to
        # quit, so these end in TLS_E_EOF.
        c = Case('OpenSSL s_server: key updates, asked for and not', None, final=E_EOF,
                 data=b'under the first key\nunder the second\nunder the third\n', again=b'and this after the update\n')
        record(c, lambda: record_s_server(
            exe, c, workdir, [],
            [(0.7, b'K\n'), (0.3, b'under the first key\n'), (0.3, b'k\n'), (0.3, b'under the second\n'),
             (0.3, b'K\n'), (0.3, b'under the third\n'), (0.5, b'Q\n')], [c.request, c.again]))
        c = Case('OpenSSL s_server: AES-128-GCM only, and a key update', None, final=E_EOF,
                 data=b'sealed with AES-128-GCM\nand under the next key\n', again=b'and this after the update\n')
        record(c, lambda: record_s_server(
            exe, c, workdir, ['-ciphersuites', 'TLS_AES_128_GCM_SHA256'],
            [(0.7, b'sealed with AES-128-GCM\n'), (0.3, b'K\n'), (0.3, b'and under the next key\n'), (0.5, b'Q\n')],
            [c.request, c.again]))

    if failed:
        print('%d of %d cases did not go as they should; tls_cases.h is left alone' % (failed, failed + len(out)))
        return 1

    with open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'tests/tls_cases.h'), 'w') as f:
        f.write('// Recorded TLS conversations for test_tls.c. Made by gen_tls_cases.py: do not edit.\n')
        f.write('// %d conversations.\n' % len(out))
        f.write('static const tls_record_t tls_records[] = {\n')
        for c, server, client in out:
            f.write('  { "%s", "%s", %du,\n' % (c.name.replace('"', '\\"'), c.host, c.seed))
            for b in (c.request, c.again, server):
                f.write(hex_lines(b).lstrip() if not b else hex_lines(b))
                f.write(',\n')
            f.write('      "%s", %d,\n' % (hashlib.sha256(client).hexdigest(), len(client)))
            f.write(hex_lines(c.data).lstrip() if not c.data else hex_lines(c.data))
            f.write(',\n      %d, %d, %d, %d, %d, "%s" },\n' % (
                c.connect, c.final, 1 if c.abuse else 0, ignorable_bytes(server), c.verdict,
                hashlib.sha256(c.result.peer).hexdigest() if c.result.asked else ''))
        f.write('};\n')
    total = sum(len(s) for _, s, _ in out)
    print('%d conversations recorded, %d bytes from the servers' % (len(out), total))
    return 0


if __name__ == '__main__':
    sys.exit(main())

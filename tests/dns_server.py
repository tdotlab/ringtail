"""A DNS server for the tests, with a zone of names that each misbehave
in their own way. Zone.handle() turns a query into the datagrams to
send back (none, one, or a forgery before the real one); Server runs
it on a host UDP port, which a guest on QEMU's user network reaches as
10.0.2.2:PORT. The peer tests call handle() directly.
"""
import socket
import struct
import threading

A, CNAME, PTR = 1, 5, 12
NOERROR, FORMERR, SERVFAIL, NXDOMAIN, REFUSED = 0, 1, 2, 3, 5


def wire(name):
    """A name in wire format, uncompressed. Labels are split on dots."""
    out = b""
    for label in name.encode("latin-1").split(b".") if name else []:
        out += bytes([len(label)]) + label
    return out + b"\0"


def rr(owner, typ, rdata, ttl=300):
    return owner + struct.pack("!HHIH", typ, 1, ttl, len(rdata)) + rdata


def ip(s):
    return socket.inet_aton(s)


def parse_query(q):
    """(id, flags, name, qtype, qclass, question bytes) or None."""
    if len(q) < 12:
        return None
    ident, flags, qd = struct.unpack("!HHH", q[:6])
    if qd != 1:
        return None
    i, labels = 12, []
    while i < len(q) and q[i]:
        n = q[i]
        if n > 63 or i + 1 + n > len(q):
            return None
        labels.append(q[i + 1:i + 1 + n].decode("latin-1"))
        i += 1 + n
    if i + 5 > len(q):
        return None
    qtype, qclass = struct.unpack("!HH", q[i + 1:i + 5])
    return ident, flags, ".".join(labels), qtype, qclass, q[12:i + 5]


class Zone:
    """The test names. Answers for names it does not know are NXDOMAIN."""

    def __init__(self, extra=None):
        self.asked = []     # (name, qtype, id, flags) of every query seen
        self.tries = {}     # name -> how many times it was asked
        self.extra = dict(extra or {})  # name -> address, for names a test wants to add

    def reply(self, qq, rcode=NOERROR, answers=(), tc=False, ident=None, question=None, an=None):
        ident0, flags, _, _, _, question0 = qq
        f = 0x8000 | (flags & 0x0100) | 0x0080 | rcode | (0x0200 if tc else 0)
        hdr = struct.pack("!HHHHHH", ident0 if ident is None else ident, f, 1,
                          len(answers) if an is None else an, 0, 0)
        return hdr + (question0 if question is None else question) + b"".join(answers)

    def handle(self, q):
        qq = parse_query(q)
        if not qq:
            return []
        ident, flags, name, qtype, qclass, question = qq
        low = name.lower()
        self.asked.append((low, qtype, ident, flags))
        self.tries[low] = self.tries.get(low, 0) + 1
        own = b"\xc0\x0c"  # a pointer to the question's name
        r = lambda *a, **k: [self.reply(qq, *a, **k)]

        if qtype == A and low in self.extra:
            return r(answers=[rr(own, A, ip(self.extra[low]))])
        if qtype == PTR:
            if low == "1.2.0.192.in-addr.arpa":
                return r(answers=[rr(own, PTR, wire("a.test"))])
            if low == "2.2.0.192.in-addr.arpa":
                return r()  # the address is known, but has no name
            if low == "3.2.0.192.in-addr.arpa":  # through an alias, as RFC 2317 delegation does
                return r(answers=[rr(own, CNAME, wire("3.0/26.2.0.192.in-addr.arpa")),
                                  rr(wire("3.0/26.2.0.192.in-addr.arpa"), PTR, wire("c.test"))])
            return r(NXDOMAIN)
        if qtype != A:
            return r(4)  # NOTIMP
        if low == "a.test":
            return r(answers=[rr(own, A, ip("192.0.2.1"))])
        if low == "multi.test":  # three, one of them twice, and one of another class
            return r(answers=[rr(own, A, ip("192.0.2.1")), rr(own, A, ip("192.0.2.2")),
                              rr(own, A, ip("192.0.2.2")),
                              own + struct.pack("!HHIH", A, 3, 60, 4) + ip("192.0.2.66"),
                              rr(own, A, ip("192.0.2.3"))])
        if low == "alias.test":  # the alias and where it leads, compressed against the alias
            target = 12 + len(question) + 12  # where the CNAME's data starts
            return r(answers=[rr(own, CNAME, wire("a.test")),
                              rr(struct.pack("!BB", 0xC0, target), A, ip("192.0.2.1"))])
        if low == "lone.test":  # only the alias: the resolver must ask again
            return r(answers=[rr(own, CNAME, wire("a.test"))])
        if low in ("loop.test", "loop2.test"):  # each the other's alias, one hop per answer
            return r(answers=[rr(own, CNAME, wire("loop2.test" if low == "loop.test" else "loop.test"))])
        if low == "noaddr.test":
            return r()
        if low == "fail.test":
            return r(SERVFAIL)
        if low == "refused.test":
            return r(REFUSED)
        if low == "slow.test":
            return []
        if low == "late.test":  # the first try goes unanswered
            return [] if self.tries[low] == 1 else r(answers=[rr(own, A, ip("192.0.2.5"))])
        if low == "forged.test":  # a wrong ID and a wrong question first, then the truth
            return [self.reply(qq, answers=[rr(own, A, ip("6.6.6.6"))], ident=ident ^ 0x5A5A),
                    self.reply(qq, answers=[rr(wire("a.test"), A, ip("6.6.6.6"))], question=wire("a.test") + question[-4:]),
                    self.reply(qq, answers=[rr(own, A, ip("192.0.2.9"))])]
        if low == "big.test":  # truncated with nothing whole in it
            return r(tc=True)
        if low == "ptrloop.test":  # an answer whose name points at itself
            at = 12 + len(question)
            return r(answers=[struct.pack("!BB", 0xC0 | at >> 8, at & 255) + struct.pack("!HHIH", A, 1, 1, 4) + ip("1.1.1.1")])
        if low == "shout.test":  # the question and the answer come back in capitals
            return r(answers=[rr(wire("SHOUT.TEST"), A, ip("192.0.2.4"))], question=wire("SHOUT.TEST") + question[-4:])
        if low == "evil.test":  # an alias to a name a terminal should not print raw
            bad = wire("e\x1b[2Jx.test")
            return r(answers=[rr(own, CNAME, bad), rr(bad, A, ip("192.0.2.7"))])
        if low == "lies.test":  # an A record four bytes it is not
            return r(answers=[own + struct.pack("!HHIH", A, 1, 1, 5) + ip("1.1.1.1") + b"x"])
        return r(NXDOMAIN)


class Server:
    """Zone on a host UDP port."""

    def __init__(self, zone=None):
        self.zone = zone or Zone()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.1)
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while self.running:
            try:
                q, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            for d in self.zone.handle(q):
                self.sock.sendto(d, addr)

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()

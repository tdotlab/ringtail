"""A second machine on RingTail's network, played by this script.

QEMU's socket network carries each Ethernet frame as one UDP datagram,
so the test can put any frame it likes on the wire, well formed or
not, and see exactly what the kernel sends back. A thread answers the
guest's ARP requests and pings the way a host (10.0.2.7) and a router
(10.0.2.2) would; every other frame the guest sends is queued for the
test to look at.
"""
import queue
import random
import re
import socket
import struct
import threading
import time

GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000007")
GW_MAC = bytes.fromhex("020000000002")
BROADCAST = b"\xff" * 6
GUEST_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 7])
GW_IP = bytes([10, 0, 2, 2])
ETH_ARP, ETH_IP = 0x0806, 0x0800


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def eth(dst, src, typ, payload):
    return dst + src + struct.pack("!H", typ) + payload


def arp(op, sha, spa, tha, tpa):
    return struct.pack("!HHBBH", 1, ETH_IP, 6, 4, op) + sha + spa + tha + tpa


def ipv4(src, dst, payload, proto=1, ttl=64, ident=1, frag=0, opts=b"", total=None, bad_csum=False):
    ihl = 5 + len(opts) // 4
    h = struct.pack("!BBHHHBBH4s4s", 0x40 | ihl, 0, total if total is not None else ihl * 4 + len(payload),
                    ident, frag, ttl, proto, 0, src, dst) + opts
    c = csum(h) ^ (0x5555 if bad_csum else 0)
    return h[:10] + struct.pack("!H", c) + h[12:] + payload


def icmp(typ, code, rest, data, bad_csum=False):
    m = struct.pack("!BBH", typ, code, 0) + rest + data
    c = csum(m) ^ (0x1111 if bad_csum else 0)
    return m[:2] + struct.pack("!H", c) + m[4:]


def echo_request(ident, seq, data, **kw):
    return icmp(8, 0, struct.pack("!HH", ident, seq), data, **kw)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Peer:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.qemu_port = free_udp_port()
        self.frames = queue.Queue()
        # How the guest's pings are answered: echo, silent, unreachable
        # (a router's "host unreachable") or garble (damaged data).
        self.mode = "echo"
        self.arp_asked = []    # addresses the guest asked ARP for
        self.guest_pings = []  # (Ethernet destination, IP destination) of its echo requests
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def qemu_args(self):
        return ["-netdev", f"socket,id=n0,udp=127.0.0.1:{self.port},localaddr=127.0.0.1:{self.qemu_port}",
                "-device", "e1000,netdev=n0,mac=52:54:00:12:34:56"]

    def send(self, frame):
        self.sock.sendto(frame, ("127.0.0.1", self.qemu_port))

    def _serve(self):
        self.sock.settimeout(0.1)
        while self.running:
            try:
                f, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not self._answer(f):
                self.frames.put(f)

    def _answer(self, f):
        typ = struct.unpack("!H", f[12:14])[0]
        if typ == ETH_ARP and f[20:22] == b"\0\1" and f[28:32] == GUEST_IP:
            tpa = f[38:42]
            self.arp_asked.append(tpa)
            mac = {PEER_IP: PEER_MAC, GW_IP: GW_MAC}.get(tpa)
            if mac:
                self.send(eth(f[6:12], mac, ETH_ARP, arp(2, mac, tpa, f[22:28], GUEST_IP)))
            return True
        if typ == ETH_IP and f[23] == 1 and f[34] == 8 and f[26:30] == GUEST_IP:
            total = struct.unpack("!H", f[16:18])[0]
            req = f[34:14 + total]
            dst = f[30:34]
            self.guest_pings.append((f[0:6], dst))
            if self.mode == "silent":
                return True
            if self.mode == "unreachable":  # quotes the header and 8 bytes, as routers do
                reply = ipv4(GW_IP, GUEST_IP, icmp(3, 1, b"\0" * 4, f[14:14 + 20 + 8]))
            else:
                data = bytearray(req[8:])
                if self.mode == "garble" and data:
                    data[len(data) // 2] ^= 0x40
                reply = ipv4(dst, GUEST_IP, icmp(0, 0, req[4:8], bytes(data)), ttl=77)
            self.send(eth(f[6:12], f[0:6], ETH_IP, reply))
            return True
        return False

    def expect(self, match=lambda f: True, timeout=2.0):
        """The next frame from the guest that match accepts, or None."""
        end = time.time() + timeout
        while (left := end - time.time()) > 0:
            try:
                f = self.frames.get(timeout=left)
            except queue.Empty:
                return None
            if match(f):
                return f
        return None

    def drain(self, quiet=0.3):
        """Forget what the guest sent, once it has been quiet a while."""
        while True:
            try:
                self.frames.get(timeout=quiet)
            except queue.Empty:
                return

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()


def is_echo_reply(ident, seq):
    return lambda f: (f[12:14] == b"\x08\x00" and f[23] == 1 and f[34] == 0 and
                      f[38:40] == struct.pack("!H", ident) and f[40:42] == struct.pack("!H", seq))


def counters(net_out):
    m = re.search(r"Frames:\s+(\d+) in, (\d+) out; (\d+) dropped, (\d+) for others, (\d+) fragments", net_out)
    return dict(zip(["in", "out", "dropped", "others", "fragments"], map(int, m.groups()))) if m else None


def fuzz_frames(rng, n):
    """Frames that are wrong in every way a lazy parser trips over."""
    good = eth(GUEST_MAC, PEER_MAC, ETH_IP, ipv4(PEER_IP, GUEST_IP, echo_request(9, 9, bytes(64))))
    good_arp = eth(BROADCAST, PEER_MAC, ETH_ARP, arp(1, PEER_MAC, PEER_IP, bytes(6), GUEST_IP))
    for _ in range(n):
        kind = rng.randrange(5)
        if kind == 0:  # noise after a header the card accepts
            f = rng.choice([GUEST_MAC, BROADCAST]) + PEER_MAC + rng.choice([b"\x08\x00", b"\x08\x06", rng.randbytes(2)])
            f += rng.randbytes(rng.randrange(0, 1500))
        elif kind == 1:  # a good frame with bytes flipped (never the sender's MAC)
            f = bytearray(rng.choice([good, good_arp]))
            for _ in range(rng.randrange(1, 6)):
                f[rng.randrange(12, len(f))] = rng.randrange(256)
            f = bytes(f)
        elif kind == 2:  # cut short anywhere, though not to nothing: QEMU takes
            f = good[:rng.randrange(1, len(good))]  # an empty datagram as the network closing
        elif kind == 3:  # IP headers with lying lengths, but checksums that hold, so they get looked at
            opts = rng.randbytes(4 * rng.randrange(0, 11))
            body = rng.randbytes(rng.randrange(0, 200))
            h = ipv4(PEER_IP, GUEST_IP, body, proto=rng.choice([1, 1, 6, 17, rng.randrange(256)]),
                     frag=rng.choice([0, 0, 0x2000, rng.randrange(65536)]), opts=opts,
                     total=rng.choice([None, rng.randrange(65536), rng.randrange(60)]))
            h = bytes([rng.choice([h[0], 0x40 | rng.randrange(16), rng.randrange(256)])]) + h[1:]
            hl = (h[0] & 15) * 4
            if 20 <= hl <= len(h):
                h = h[:10] + b"\0\0" + h[12:]
                h = h[:10] + struct.pack("!H", csum(h[:hl])) + h[12:]
            f = eth(GUEST_MAC, PEER_MAC, ETH_IP, h)
        else:  # ICMP of every type and code, with good checksums
            m = icmp(rng.randrange(256), rng.randrange(256), rng.randbytes(4), rng.randbytes(rng.randrange(0, 100)))
            f = eth(GUEST_MAC, PEER_MAC, ETH_IP, ipv4(PEER_IP, GUEST_IP, m))
        yield f


def peer_tests(make_machine, prompt, t):
    p = Peer()
    m = make_machine(p.qemu_args())
    try:
        _peer_tests(m, p, prompt, t)
    finally:
        p.close()
    return m


def _peer_tests(m, p, prompt, t):
    boot = m.expect(prompt, 30)
    t.check("[peer] boots with the card on a socket network", "Network: e1000 82540EM 52:54:00:12:34:56" in boot, boot)

    # ARP: we ask who 10.0.2.15 is.
    p.send(eth(BROADCAST, PEER_MAC, ETH_ARP, arp(1, PEER_MAC, PEER_IP, bytes(6), GUEST_IP)))
    f = p.expect(lambda f: f[12:14] == b"\x08\x06", timeout=3)  # QEMU's e1000 sits on frames for 1 s after start
    t.check("[peer] answers an ARP request", f is not None)
    if f:
        t.check("[peer] to the asker, with its own MAC and address",
                f[0:6] == PEER_MAC and f[6:12] == GUEST_MAC and
                f[14:42] == arp(2, GUEST_MAC, GUEST_IP, PEER_MAC, PEER_IP), f.hex())
        t.check("[peer] in a frame padded to the minimum", len(f) == 60, str(len(f)))
    p.send(eth(BROADCAST, PEER_MAC, ETH_ARP, arp(1, PEER_MAC, PEER_IP, bytes(6), bytes([10, 0, 2, 99]))))
    t.check("[peer] but not for someone else's address", p.expect(timeout=0.5) is None)

    # Pings at it, in every size and shape that ought to work.
    def ping_it(seq, data, opts=b"", pad=b""):
        p.send(eth(GUEST_MAC, PEER_MAC, ETH_IP, ipv4(PEER_IP, GUEST_IP, echo_request(0x4242, seq, data), opts=opts) + pad))
        return p.expect(is_echo_reply(0x4242, seq))

    rng = random.Random(1980)
    for seq, size in enumerate([56, 0, 1, 33, 1472], 1):
        data = rng.randbytes(size)
        f = ping_it(seq, data)
        ok = f is not None
        if ok:
            ip, total = f[14:34], struct.unpack("!H", f[16:18])[0]
            ok = (f[0:6] == PEER_MAC and f[6:12] == GUEST_MAC and ip[0] == 0x45 and total == 28 + size and
                  ip[12:16] == GUEST_IP and ip[16:20] == PEER_IP and ip[8] == 64 and csum(ip) == 0 and
                  csum(f[34:14 + total]) == 0 and f[42:14 + total] == data)
        t.check(f"[peer] answers a {size}-byte ping with the same data and good checksums", ok, f.hex() if f else "no reply")
    f = ping_it(6, b"options", opts=b"\x01\x01\x01\x00")
    t.check("[peer] reads past IP options", f is not None and f[14] == 0x45 and f[42:49] == b"options",
            f.hex() if f else "no reply")
    f = ping_it(7, b"short", pad=b"\xee" * 30)
    t.check("[peer] and does not take the frame's padding for data",
            f is not None and struct.unpack("!H", f[16:18])[0] == 33 and f[42:47] == b"short", f.hex() if f else "no reply")

    before = counters(m.run("net"))
    # Pings that must not be answered.
    bad = [
        ("a bad IP checksum", ipv4(PEER_IP, GUEST_IP, echo_request(1, 1, b"x" * 20), bad_csum=True)),
        ("a bad ICMP checksum", ipv4(PEER_IP, GUEST_IP, echo_request(1, 2, b"x" * 20, bad_csum=True))),
        ("a packet longer than its frame", ipv4(PEER_IP, GUEST_IP, echo_request(1, 3, b"x" * 20), total=200)),
        ("a first fragment", ipv4(PEER_IP, GUEST_IP, echo_request(1, 4, b"x" * 24), frag=0x2000)),
        ("a later fragment", ipv4(PEER_IP, GUEST_IP, echo_request(1, 5, b"x" * 24), frag=3)),
        ("a ping for another address", ipv4(PEER_IP, bytes([10, 0, 2, 99]), echo_request(1, 6, b"x"))),
        ("a ping to the broadcast address", ipv4(PEER_IP, bytes([10, 0, 2, 255]), echo_request(1, 7, b"x"))),
        ("UDP, which nothing here speaks yet", ipv4(PEER_IP, GUEST_IP, b"\x30\x39\x00\x07\x00\x0c\x00\x00data", proto=17)),
        ("a header shorter than 20 bytes", bytes([0x44]) + ipv4(PEER_IP, GUEST_IP, echo_request(1, 8, b"x"))[1:]),
        ("IPv6", bytes([0x60]) + bytes(39)),
    ]
    for what, packet in bad:
        dst = BROADCAST if "broadcast" in what else GUEST_MAC
        p.send(eth(dst, PEER_MAC, 0x86DD if what == "IPv6" else ETH_IP, packet))
        t.check(f"[peer] ignores {what}", p.expect(timeout=0.4) is None)
    p.send(eth(GUEST_MAC, b"\x03" + PEER_MAC[1:], ETH_IP, ipv4(PEER_IP, GUEST_IP, echo_request(1, 9, b"x"))))
    t.check("[peer] and a frame from a group address", p.expect(timeout=0.4) is None)
    after = counters(m.run("net"))
    t.check("[peer] and counts what it ignored", before and after and
            after["in"] - before["in"] == len(bad) + 1 and after["fragments"] - before["fragments"] == 2 and
            after["others"] - before["others"] == 1 and after["dropped"] - before["dropped"] == len(bad) - 3 + 1,
            f"{before} -> {after}")

    # The guest pings us. It learned our MAC from our ARP request.
    out = m.run("ping -c 2 10.0.2.7", timeout=20)
    t.check("[peer] ping reaches a host on the network", "2 sent, 2 received" in out and "ttl=77" in out, out)
    t.check("[peer] sent to the host's MAC", p.guest_pings[-2:] == [(PEER_MAC, PEER_IP)] * 2, str(p.guest_pings))
    out = m.run("ping -c 1 8.8.8.8", timeout=20)
    t.check("[peer] and one off the network goes through the router", "1 received" in out and
            GW_IP in p.arp_asked and p.guest_pings[-1] == (GW_MAC, bytes([8, 8, 8, 8])), out)
    out = m.run("net")
    t.check("[peer] net lists both neighbours", "10.0.2.7 is 02:00:00:00:00:07" in out and
            "10.0.2.2 is 02:00:00:00:00:02" in out, out)
    p.mode = "silent"
    start = time.time()
    out = m.run("ping -c 1 -W 300 10.0.2.7")
    took = time.time() - start
    t.check("[peer] an unanswered ping times out", "seq=1: timed out" in out and "[exit status 1]" in out, out)
    t.check("[peer] when -W says", took < 1.5, "%.2f s" % took)
    p.mode = "unreachable"
    out = m.run("ping -c 1 8.8.8.8")
    t.check("[peer] a router's host unreachable ends the ping", "seq=1: host unreachable" in out, out)
    p.mode = "garble"
    out = m.run("ping -c 1 10.0.2.7")
    t.check("[peer] a damaged reply is caught", "seq=1: the reply came back damaged" in out, out)
    p.mode = "echo"

    # Garbage, fast enough to overrun the card's ring, then a ping to
    # show that nothing broke and nothing leaked.
    mem_before = m.run("mem")
    sent = 0
    for f in fuzz_frames(random.Random(7), 3000):
        p.send(f)
        sent += 1
        if sent % 200 == 0:
            time.sleep(0.05)
    p.drain()
    f = None
    for seq in range(100, 105):  # the ring may take a moment to drain
        if (f := ping_it(seq, b"still here")):
            break
    t.check("[peer] 3000 frames of garbage later it still answers", f is not None and f[42:52] == b"still here")
    out = m.run("echo alive")
    t.check("[peer] and the shell is fine", "alive" in out, out)
    t.check("[peer] and no memory went missing", m.run("mem") == mem_before, mem_before)
    out = m.run("net")
    c = counters(out)
    t.check("[peer] net counts the flood", c and c["in"] > 1000, out)
    m.send("poweroff\r")
    m.close()

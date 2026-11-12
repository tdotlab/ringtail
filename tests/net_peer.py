"""A second machine on RingTail's network, played by this script.

QEMU's socket network carries each Ethernet frame as one UDP datagram,
so the test can put any frame it likes on the wire, well formed or
not, and see exactly what the kernel sends back. A thread answers the
guest's ARP requests and pings the way a host (10.0.2.7) and a router
(10.0.2.2) would, and the router is the network's DHCP server and its
DNS server too; every other frame the guest sends is queued for the
test to look at.
"""
import queue
import random
import re
import socket
import struct
import threading
import time

import dns_server
import ntp_server

GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000007")
GW_MAC = bytes.fromhex("020000000002")
BROADCAST = b"\xff" * 6
GUEST_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 7])
GW_IP = bytes([10, 0, 2, 2])
ETH_ARP, ETH_IP = 0x0806, 0x0800
ANY = bytes(4)
ALL = b"\xff" * 4
OUR_NET = bytes([10, 0, 2])


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


def udp(src, dst, sport, dport, data, check="good", length=None):
    """A UDP datagram. check is good, none (0, which means unchecked) or bad."""
    n = 8 + len(data)
    u = struct.pack("!HHHH", sport, dport, n if length is None else length, 0) + data
    if check == "none":
        return u
    c = csum(src + dst + struct.pack("!BBH", 0, 17, n) + u) or 0xFFFF
    if check == "bad":
        c ^= 0x0101
    return u[:6] + struct.pack("!H", c) + u[8:]


def udp_ok(f):
    """Whether a frame's UDP checksum holds (or was not given)."""
    hl = (f[14] & 15) * 4
    u = f[14 + hl:14 + struct.unpack("!H", f[16:18])[0]]
    return u[6:8] == b"\0\0" or csum(f[26:30] + f[30:34] + struct.pack("!BBH", 0, 17, len(u)) + u) == 0


# DHCP, the server's half.
DHCP_COOKIE = bytes([99, 130, 83, 99])


def dhcp_client_msg(f):
    """What a DHCP client sent, from a frame to port 67, or None."""
    p = f[42:]
    if len(p) < 240 or p[236:240] != DHCP_COOKIE:
        return None
    opts, i = {}, 240
    while i < len(p) and p[i] != 255:
        if p[i] == 0:
            i += 1
            continue
        if i + 2 > len(p):
            return None
        opts[p[i]] = p[i + 2:i + 2 + p[i + 1]]
        i += 2 + p[i + 1]
    return {"op": p[0], "xid": p[4:8], "secs": struct.unpack("!H", p[8:10])[0],
            "flags": struct.unpack("!H", p[10:12])[0], "ciaddr": p[12:16], "chaddr": p[28:34],
            "type": opts.get(53, b"\0")[0], "opts": opts, "len": len(p), "sport": struct.unpack("!H", f[34:36])[0],
            "eth_dst": f[0:6], "ip_src": f[26:30], "ip_dst": f[30:34], "time": time.time()}


def dhcp_server_msg(typ, xid, yiaddr, chaddr, server=GW_IP, op=2, opts=b"", flags=0x8000):
    p = struct.pack("!BBBB4sHH4s4s4s4s16s64s128s", op, 1, 6, 0, xid, 0, flags, ANY, yiaddr, ANY, ANY,
                    chaddr, b"", b"") + DHCP_COOKIE
    p += bytes([53, 1, typ])
    if server:
        p += bytes([54, 4]) + server
    return p + opts + b"\xff"


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
        self.ping_sources = []  # and the address each came from
        # DHCP: normal, silent, nak, no-renew (ignores requests sent
        # straight to it, answers broadcast ones), or evil (bad offers
        # and acks first, then a good one).
        self.dhcp_mode = "normal"
        self.dhcp_log = []     # every DHCP message the guest sent, as dhcp_client_msg gives it
        self.dhcp_sent = []    # (type, xid) of every answer
        self.lease, self.t1, self.t2 = 3600, None, None
        self.dns_servers = [GW_IP]
        self.domain = b"lan"
        # Time: DHCP names 10.0.2.2 (option 42), which answers SNTP with
        # the host's time.
        self.ntp_servers = [GW_IP]
        self.ntp_queries = []  # (source port, request) of every SNTP request
        # DNS at 10.0.2.2:53 answers from the test zone; 10.0.2.7:53 is
        # a server that fails everything, for a resolver to move on from.
        self.zone = dns_server.Zone(extra={"printer.lan": "10.0.2.7", "gw.lan": "10.0.2.2"})
        self.dns_queries = []  # (server, source port, Ethernet destination) of every DNS question
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
        if typ == ETH_ARP and f[20:22] == b"\0\1" and f[28:31] == OUR_NET:
            tpa = f[38:42]
            self.arp_asked.append(tpa)
            mac = {PEER_IP: PEER_MAC, GW_IP: GW_MAC}.get(tpa)
            if mac:
                self.send(eth(f[6:12], mac, ETH_ARP, arp(2, mac, tpa, f[22:28], f[28:32])))
            return True
        if typ == ETH_IP and f[23] == 17 and len(f) >= 42:
            dport = struct.unpack("!H", f[36:38])[0]
            if dport == 67:
                self._dhcp(f)
                return True
            if dport == 53 and f[30:34] in (GW_IP, PEER_IP):
                self._dns(f)
                return True
            if dport == 123 and f[30:34] == GW_IP:
                self._ntp(f)
                return True
            return False
        if typ == ETH_IP and f[23] == 1 and f[34] == 8 and f[26:29] == OUR_NET:
            total = struct.unpack("!H", f[16:18])[0]
            req = f[34:14 + total]
            dst = f[30:34]
            self.guest_pings.append((f[0:6], dst))
            self.ping_sources.append(f[26:30])
            if self.mode == "silent":
                return True
            if self.mode == "unreachable":  # quotes the header and 8 bytes, as routers do
                reply = ipv4(GW_IP, GUEST_IP, icmp(3, 1, b"\0" * 4, f[14:14 + 20 + 8]))
            else:
                data = bytearray(req[8:])
                if self.mode == "garble" and data:
                    data[len(data) // 2] ^= 0x40
                reply = ipv4(dst, f[26:30], icmp(0, 0, req[4:8], bytes(data)), ttl=77)
            self.send(eth(f[6:12], f[0:6], ETH_IP, reply))
            return True
        return False

    def _dhcp(self, f):
        m = dhcp_client_msg(f)
        if not m:
            return
        self.dhcp_log.append(m)
        mode = self.dhcp_mode
        if mode == "silent" or m["op"] != 1:
            return
        if m["type"] == 3 and mode == "no-renew" and m["ip_dst"] != ALL:
            return
        opts = bytes([1, 4, 255, 255, 255, 0, 3, 4]) + GW_IP
        opts += bytes([6, 4 * len(self.dns_servers)]) + b"".join(self.dns_servers)
        if self.domain:
            opts += bytes([15, len(self.domain)]) + self.domain
        if self.ntp_servers:
            opts += bytes([42, 4 * len(self.ntp_servers)]) + b"".join(self.ntp_servers)
        opts += bytes([51, 4]) + struct.pack("!I", self.lease)
        if self.t1 is not None:
            opts += bytes([58, 4]) + struct.pack("!I", self.t1)
        if self.t2 is not None:
            opts += bytes([59, 4]) + struct.pack("!I", self.t2)
        xid, mac = m["xid"], m["chaddr"]
        out = []
        if m["type"] == 1:
            if mode == "evil":  # offers the client must not take
                out += [(2, dhcp_server_msg(2, bytes(b ^ 1 for b in xid), bytes([10, 0, 2, 66]), mac, opts=opts)),
                        (2, dhcp_server_msg(2, xid, bytes([10, 0, 2, 67]), PEER_MAC, opts=opts)),
                        (2, dhcp_server_msg(2, xid, ALL, mac, opts=opts)),
                        (2, dhcp_server_msg(2, xid, bytes([10, 0, 2, 68]), mac, op=1, opts=opts)),
                        (2, dhcp_server_msg(2, xid, bytes([10, 0, 2, 69]), mac, server=None, opts=opts))]
            out.append((2, dhcp_server_msg(2, xid, GUEST_IP, mac, opts=opts)))
        elif m["type"] == 3:
            want = m["opts"].get(50, m["ciaddr"])
            if mode == "nak":
                out.append((6, dhcp_server_msg(6, xid, ANY, mac)))
            else:
                if mode == "evil":  # another server's answer, to someone else's request
                    out.append((5, dhcp_server_msg(5, xid, bytes([10, 0, 2, 70]), mac,
                                                   server=bytes([10, 0, 2, 99]), opts=opts)))
                out.append((5, dhcp_server_msg(5, xid, want, mac, opts=opts)))
        unicast = m["ciaddr"] != ANY and not m["flags"] & 0x8000
        for typ, p in out:
            self.dhcp_sent.append((typ, xid))
            dst_ip, dst_mac = (m["ciaddr"], mac) if unicast else (ALL, BROADCAST)
            self.send(eth(dst_mac, GW_MAC, ETH_IP, ipv4(GW_IP, dst_ip, udp(GW_IP, dst_ip, 67, 68, p), proto=17)))

    def _dns(self, f):
        server, client = f[30:34], f[26:30]
        sport = struct.unpack("!H", f[34:36])[0]
        total = struct.unpack("!H", f[16:18])[0]
        q = f[42:14 + total]
        self.dns_queries.append((server, sport, f[0:6]))

        def reply(data, src=server, port=53):
            self.send(eth(f[6:12], f[0:6], ETH_IP, ipv4(src, client, udp(src, client, port, sport, data), proto=17)))
        if server == PEER_IP:  # the server that always fails
            qq = dns_server.parse_query(q)
            if qq:
                reply(self.zone.reply(qq, dns_server.SERVFAIL))
            return
        qq = dns_server.parse_query(q)
        if qq and qq[2].lower() == "wrongsrc.test":
            # The right ID and question, but from the wrong address, then the wrong port.
            fake = self.zone.reply(qq, answers=[dns_server.rr(b"\xc0\x0c", dns_server.A, dns_server.ip("6.6.6.6"))])
            reply(fake, src=PEER_IP)
            reply(fake, port=5353)
            reply(self.zone.reply(qq, answers=[dns_server.rr(b"\xc0\x0c", dns_server.A, dns_server.ip("192.0.2.8"))]))
            return
        for d in self.zone.handle(q):
            reply(d)

    def _ntp(self, f):
        total = struct.unpack("!H", f[16:18])[0]
        q, sport = f[42:14 + total], struct.unpack("!H", f[34:36])[0]
        self.ntp_queries.append((sport, q))
        if len(q) < 48:
            return
        now = time.time()
        reply = (struct.pack("!BBbbII", 4 << 3 | 4, 2, 6, -20, 0x100, 0x200) + b"\xc0\x00\x02\x01" +
                 struct.pack("!Q", ntp_server.ntp_ts(now - 30)) + q[40:48] + struct.pack("!QQ", ntp_server.ntp_ts(now),
                 ntp_server.ntp_ts(time.time())))
        self.send(eth(f[6:12], GW_MAC, ETH_IP, ipv4(GW_IP, f[26:30], udp(GW_IP, f[26:30], 123, sport, reply), proto=17)))

    def dhcp_wait(self, count, timeout):
        """Waits until the guest has sent count DHCP messages in all."""
        end = time.time() + timeout
        while len(self.dhcp_log) < count and time.time() < end:
            time.sleep(0.05)
        return len(self.dhcp_log) >= count

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


def udp_counters(net_out):
    m = re.search(r"UDP:\s+(\d+) sockets; (\d+) datagrams in, (\d+) out, (\d+) to closed ports, (\d+) lost", net_out)
    return dict(zip(["sockets", "in", "out", "closed", "lost"], map(int, m.groups()))) if m else None


def plain(out):
    return re.sub(r"\x1b\[[0-9;]*m", "", out).replace("\r\n", "\n")


def counters(net_out):
    m = re.search(r"Frames:\s+(\d+) in, (\d+) out; (\d+) dropped, (\d+) for others, (\d+) fragments", net_out)
    return dict(zip(["in", "out", "dropped", "others", "fragments"], map(int, m.groups()))) if m else None


def fuzz_frames(rng, n):
    """Frames that are wrong in every way a lazy parser trips over."""
    good = eth(GUEST_MAC, PEER_MAC, ETH_IP, ipv4(PEER_IP, GUEST_IP, echo_request(9, 9, bytes(64))))
    good_arp = eth(BROADCAST, PEER_MAC, ETH_ARP, arp(1, PEER_MAC, PEER_IP, bytes(6), GUEST_IP))
    for _ in range(n):
        kind = rng.randrange(6)
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
        elif kind == 4:  # ICMP of every type and code, with good checksums
            m = icmp(rng.randrange(256), rng.randrange(256), rng.randbytes(4), rng.randbytes(rng.randrange(0, 100)))
            f = eth(GUEST_MAC, PEER_MAC, ETH_IP, ipv4(PEER_IP, GUEST_IP, m))
        else:  # UDP to the ports that are listened on, and DHCP answers that are not quite
            sport, dport = rng.randrange(65536), rng.choice([67, 68, 68, 53, 9, rng.randrange(65536)])
            data = rng.randbytes(rng.randrange(0, 600))
            if dport == 68:
                sport = 67
                opts = bytes(rng.choice([1, 3, 6, 15, 51, 54, 58, 59, rng.randrange(256)]) if i % 2 == 0 else
                             rng.randrange(256) for i in range(rng.randrange(0, 40)))
                data = dhcp_server_msg(rng.choice([2, 5, 6, rng.randrange(256)]), rng.randbytes(4),
                                       rng.choice([GUEST_IP, rng.randbytes(4)]), GUEST_MAC, opts=opts)
                if rng.random() < 0.3:
                    data = data[:rng.randrange(len(data))]
            dst = rng.choice([GUEST_IP, GUEST_IP, bytes([10, 0, 2, 255]), ALL])
            u = udp(PEER_IP, dst, sport, dport, data, check=rng.choice(["good", "good", "none", "bad"]),
                    length=rng.choice([None] * 6 + [rng.randrange(65536), rng.randrange(8)]))
            f = eth(GUEST_MAC if dst == GUEST_IP else BROADCAST, PEER_MAC, ETH_IP, ipv4(PEER_IP, dst, u, proto=17))
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
    boot = plain(m.expect(prompt, 30))
    t.check("[peer] boots with the card on a socket network", "Network: e1000 82540EM 52:54:00:12:34:56" in boot, boot)
    dhcp_boot_tests(p, t, boot)
    out = plain(m.run("net"))
    t.check("[peer] net shows the time server", "\nTime:     10.0.2.2\n" in out, out)
    n = len(p.ntp_queries)
    out = plain(m.run("sntp -q", timeout=20))
    t.check("[peer] and sntp asks it when told no server", re.search(r"^10\.0\.2\.2: stratum 2, offset [+-]\d", out, re.M)
            and len(p.ntp_queries) == n + 1 and not p.dns_queries, out)

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
        ("UDP with a bad checksum", ipv4(PEER_IP, GUEST_IP, udp(PEER_IP, GUEST_IP, 12345, 7, b"data", check="bad"), proto=17)),
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

    udp_tests(m, p, prompt, t)

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

    dns_tests(m, p, t)
    dhcp_tests(m, p, t)

    # Garbage, fast enough to overrun the card's ring, then a ping to
    # show that nothing broke and nothing leaked.
    mem_before = m.run("mem")
    sent = 0
    for f in fuzz_frames(random.Random(7), 4000):
        p.send(f)
        sent += 1
        if sent % 200 == 0:
            time.sleep(0.05)
    p.drain()
    f = None
    for seq in range(100, 105):  # the ring may take a moment to drain
        if (f := ping_it(seq, b"still here")):
            break
    t.check("[peer] 4000 frames of garbage later it still answers", f is not None and f[42:52] == b"still here")
    out = m.run("echo alive")
    t.check("[peer] and the shell is fine", "alive" in out, out)
    t.check("[peer] and no memory went missing", m.run("mem") == mem_before, mem_before)
    out = plain(m.run("net"))
    c, u = counters(out), udp_counters(out)
    t.check("[peer] net counts the flood", c and c["in"] > 1000 and u and u["closed"] > 100, out)
    t.check("[peer] and the lease survived it", "Address:  10.0.2.15/24, gateway 10.0.2.2\n" in out and
            re.search(r"DHCP:     from 10\.0\.2\.2, lease \d+ m \d+ s left\n", out), out)
    m.send("poweroff\r")
    m.close()


def dhcp_boot_tests(p, t, boot):
    """What the guest said to our DHCP server on the way up."""
    t.check("[peer] takes an address from our DHCP server before the shell starts",
            "Network: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.2 (DHCP)\n" in boot, boot)
    disc = [d for d in p.dhcp_log if d["type"] == 1]
    req = [d for d in p.dhcp_log if d["type"] == 3]
    t.check("[peer] one DISCOVER and one REQUEST", len(disc) == 1 and len(req) == 1, str(p.dhcp_log))
    if not disc or not req:
        return
    d, r = disc[0], req[0]
    t.check("[peer] DISCOVER goes to everyone, from 0.0.0.0 port 68",
            d["eth_dst"] == BROADCAST and d["ip_src"] == ANY and d["ip_dst"] == ALL and d["sport"] == 68, str(d))
    t.check("[peer] asking for a broadcast answer, with our MAC and a fresh xid",
            d["op"] == 1 and d["flags"] == 0x8000 and d["chaddr"] == GUEST_MAC and d["ciaddr"] == ANY and
            d["xid"] != ANY, str(d))
    o = d["opts"]
    t.check("[peer] and the options a server needs",
            o.get(61) == b"\x01" + GUEST_MAC and set(o.get(55, b"")) >= {1, 3, 6, 15, 42, 51} and
            o.get(57) == struct.pack("!H", 1500) and o.get(12) == b"ringtail" and 50 not in o and 54 not in o, str(o))
    t.check("[peer] in the 300 bytes old servers insist on", d["len"] >= 300, str(d["len"]))
    t.check("[peer] REQUEST takes the offer from the server that made it",
            r["xid"] == d["xid"] and r["opts"].get(50) == GUEST_IP and r["opts"].get(54) == GW_IP and
            r["ciaddr"] == ANY and r["eth_dst"] == BROADCAST and r["ip_dst"] == ALL, str(r))
    t.check("[peer] then sets the clock from the time server DHCP named, before the shell starts",
            re.search(r"\nClock: (set to \S+ \S+ by 10\.0\.2\.2 \(it was|\S+ \S+, right to within half a second "
                      r"by 10\.0\.2\.2\n)", boot) and len(p.ntp_queries) == 1 and not p.dns_queries,
            boot + str(p.ntp_queries) + str(p.dns_queries))


def udp_tests(m, p, prompt, t):
    def udp_to(dst_ip, dst_mac, dport, data=b"anyone home?", **kw):
        u = udp(PEER_IP, dst_ip, 5555, dport, data, **kw)
        packet = ipv4(PEER_IP, dst_ip, u, proto=17)
        p.send(eth(dst_mac, PEER_MAC, ETH_IP, packet))
        return packet

    before = udp_counters(m.run("net"))
    sent = udp_to(GUEST_IP, GUEST_MAC, 9999)
    f = p.expect(lambda f: f[12:14] == b"\x08\x00" and f[23] == 1 and f[34] == 3, timeout=1)
    ok = f is not None
    if ok:
        total = struct.unpack("!H", f[16:18])[0]
        ok = (f[35] == 3 and f[0:6] == PEER_MAC and f[26:30] == GUEST_IP and f[30:34] == PEER_IP and
              csum(f[14:34]) == 0 and csum(f[34:14 + total]) == 0 and f[42:14 + total] == sent[:28])
    t.check("[peer] UDP to a closed port earns a port unreachable, quoting it", ok, f.hex() if f else "nothing")
    for what, dst, mac, kw in [("to the network's broadcast", bytes([10, 0, 2, 255]), BROADCAST, {}),
                               ("to everyone", ALL, BROADCAST, {}),
                               ("with a bad checksum", GUEST_IP, GUEST_MAC, {"check": "bad"}),
                               ("longer than its packet", GUEST_IP, GUEST_MAC, {"length": 200}),
                               ("shorter than its header", GUEST_IP, GUEST_MAC, {"length": 7})]:
        udp_to(dst, mac, 9999, **kw)
        t.check(f"[peer] but not one {what}", p.expect(timeout=0.4) is None)
    after = udp_counters(m.run("net"))
    t.check("[peer] net counts those for closed ports", before and after and after["closed"] - before["closed"] == 3,
            f"{before} -> {after}")

    # A program of the guest's own, echoing what we send it.
    m.send("run udptest echo 7000 4\r")
    m.expect(rb"echoing 4 datagrams on port 7000", 10)
    udp_to(GUEST_IP, GUEST_MAC, 7000, b"bad sum", check="bad")
    t.check("[peer] a damaged datagram never reaches the program", p.expect(timeout=0.5) is None)
    rng = random.Random(68)
    for what, data, check in [("a datagram", b"hello", "good"), ("one sent without a checksum", b"unchecked", "none"),
                              ("the largest that fits a frame", rng.randbytes(1472), "good"), ("an empty one", b"", "good")]:
        udp_to(GUEST_IP, GUEST_MAC, 7000, data, check=check)
        f = p.expect(lambda f: f[23] == 17 and f[34:38] == struct.pack("!HH", 7000, 5555), timeout=2)
        ok = f is not None
        if ok:
            total = struct.unpack("!H", f[16:18])[0]
            ok = (f[0:6] == PEER_MAC and f[26:30] == GUEST_IP and f[30:34] == PEER_IP and csum(f[14:34]) == 0 and
                  total == 28 + len(data) and struct.unpack("!H", f[38:40])[0] == 8 + len(data) and
                  f[42:14 + total] == data and f[40:42] != b"\0\0" and udp_ok(f))
        t.check(f"[peer] {what} comes back whole, checksummed", ok, f.hex()[:200] if f else "nothing")
    out = plain(m.expect(prompt, 10))
    t.check("[peer] and the program saw where each came from",
            all(f"{n} bytes from 10.0.2.7:5555" in out for n in (5, 9, 1472, 0)) and "bad sum" not in out, out)
    out = plain(m.run("net"))
    t.check("[peer] and closed its socket", "UDP:      0 sockets" in out, out)


def dns_tests(m, p, t):
    """The resolver, with the server and search domain DHCP gave it."""
    out = plain(m.run("net"))
    t.check("[peer] net shows the lease", re.search(r"DHCP:     from 10\.0\.2\.2, lease 59 m \d+ s left\n", out), out)
    t.check("[peer] and the DNS server and domain it came with", "DNS:      10.0.2.2, domain lan\n" in out, out)
    n = len(p.dns_queries)
    out = plain(m.run("host a.test"))
    t.check("[peer] host asks the server DHCP named", "a.test has address 192.0.2.1\n" in out, out)
    q = p.dns_queries[n] if len(p.dns_queries) > n else None
    t.check("[peer] straight to it, from a port of its own", q and q[0] == GW_IP and q[2] == GW_MAC and q[1] >= 49152, str(q))
    k = len(p.zone.asked)
    out = plain(m.run("host printer"))
    t.check("[peer] a name with no dot gets the search domain", "printer.lan has address 10.0.2.7\n" in out and
            [a[0] for a in p.zone.asked[k:]] == ["printer.lan"], out)
    k = len(p.zone.asked)
    out = plain(m.run("host nothere"))
    t.check("[peer] and if that is not it, is asked about as it is", "host: nothere: no such name" in out and
            [a[0] for a in p.zone.asked[k:]] == ["nothere.lan", "nothere"], str(p.zone.asked[k:]))
    k = len(p.zone.asked)
    out = plain(m.run("host printer."))
    t.check("[peer] a trailing dot means just that name", "no such name" in out and
            [a[0] for a in p.zone.asked[k:]] == ["printer"], str(p.zone.asked[k:]))
    out = plain(m.run("ping -c 1 printer", timeout=20))
    t.check("[peer] ping takes a name", "PING printer.lan (10.0.2.7): 56 data bytes" in out and "1 received" in out, out)
    out = plain(m.run("host wrongsrc.test"))
    t.check("[peer] an answer from the wrong address or port is ignored",
            "wrongsrc.test has address 192.0.2.8\n" in out and "6.6.6.6" not in out, out)
    ports = [q[1] for q in p.dns_queries[n:]]
    t.check("[peer] and each lookup came from a new random port", all(x >= 49152 for x in ports) and len(set(ports)) >= 5,
            str(ports))

    # Two servers, the first of which fails everything.
    p.dns_servers = [PEER_IP, GW_IP]
    out = plain(m.run("net dhcp", timeout=20))
    t.check("[peer] net dhcp gets the lease again", "Asking DHCP... 10.0.2.15/24, gateway 10.0.2.2 from 10.0.2.2" in out, out)
    disc = [d for d in p.dhcp_log if d["type"] == 1]
    t.check("[peer] asking for the address it had", disc and disc[-1]["opts"].get(50) == GUEST_IP, str(disc[-1:]))
    out = plain(m.run("net"))
    t.check("[peer] now with two DNS servers", "DNS:      10.0.2.7, 10.0.2.2, domain lan\n" in out, out)
    n = len(p.dns_queries)
    out = plain(m.run("host a.test"))
    t.check("[peer] a server that fails sends the question on to the next",
            "a.test has address 192.0.2.1\n" in out and [q[0] for q in p.dns_queries[n:]] == [PEER_IP, GW_IP],
            out + str(p.dns_queries[n:]))
    p.dns_servers = [GW_IP]
    m.run("net dhcp", timeout=20)


def dhcp_tests(m, p, t):
    """A lease's life: renewing, rebinding, running out, refusal, and a
    server that lies; then an address set by hand."""
    p.lease, p.t1, p.t2 = 40, 2, 5
    p.dhcp_mode = "no-renew"
    k = len(p.dhcp_log)
    out = plain(m.run("net dhcp", timeout=20))
    t.check("[peer] a short lease", "Asking DHCP... 10.0.2.15/24" in out, out)
    acked = p.dhcp_log[-1]["time"] if len(p.dhcp_log) >= k + 2 else time.time()
    got = p.dhcp_wait(k + 3, 5)
    r = p.dhcp_log[k + 2] if got else None
    t.check("[peer] is renewed at T1, straight to its server",
            r and r["type"] == 3 and r["eth_dst"] == GW_MAC and r["ip_src"] == GUEST_IP and r["ip_dst"] == GW_IP and
            r["ciaddr"] == GUEST_IP and r["flags"] == 0 and 50 not in r["opts"] and 54 not in r["opts"] and
            1.5 < r["time"] - acked < 3.5, str(r) + " %.2f" % ((r["time"] - acked) if r else -1))
    p.lease, p.t1, p.t2 = 3600, None, None
    got = p.dhcp_wait(k + 4, 6)
    b = p.dhcp_log[k + 3] if got else None
    t.check("[peer] and rebound at T2, by broadcast, when the server says nothing",
            b and r and b["type"] == 3 and b["eth_dst"] == BROADCAST and b["ip_dst"] == ALL and b["ip_src"] == GUEST_IP and
            b["ciaddr"] == GUEST_IP and b["xid"] == r["xid"] and b["secs"] >= 2 and 4.5 < b["time"] - acked < 6.5,
            str(b) + " %.2f" % ((b["time"] - acked) if b else -1))
    t.check("[peer] a renewal is an exchange of its own", r and r["xid"] != p.dhcp_log[k]["xid"], str(r))
    time.sleep(0.3)
    out = plain(m.run("net"))
    t.check("[peer] and the answer to the rebind is the new lease",
            re.search(r"DHCP:     from 10\.0\.2\.2, lease 59 m \d+ s left\n", out), out)

    # A lease that runs out.
    p.lease, p.t1, p.t2 = 10, 8, 9
    p.dhcp_mode = "normal"
    m.run("net dhcp", timeout=20)
    p.dhcp_mode = "silent"
    k = len(p.dhcp_log)
    time.sleep(11)
    out = plain(m.run("net"))
    t.check("[peer] when the lease runs out, the address goes", "Address:  none yet\n" in out and
            "DHCP:     asking (no answer yet)" in out and "DNS:      none" in out, out)
    disc = [d for d in p.dhcp_log[k:] if d["type"] == 1]
    t.check("[peer] and it starts over, asking for the old one back", disc and disc[0]["opts"].get(50) == GUEST_IP,
            str(p.dhcp_log[k:]))
    out = plain(m.run("ping -c 1 10.0.2.7"))
    t.check("[peer] with no address nothing goes out", "seq=1: no network address yet" in out, out)
    out = plain(m.run("host a.test"))
    t.check("[peer] and no name is looked up", "no DNS server to ask" in out, out)

    # A server that refuses: NAKs, answered more and more slowly.
    p.lease = 3600
    p.dhcp_mode = "nak"
    k, n = len(p.dhcp_log), len(p.dhcp_sent)
    out = plain(m.run("net dhcp", timeout=20))
    t.check("[peer] refused, net dhcp gives up waiting", "no answer yet; it keeps asking" in out, out)
    naks = sum(1 for x in p.dhcp_sent[n:] if x[0] == 6)
    t.check("[peer] but the client keeps asking", 3 <= naks <= 5, str(p.dhcp_sent[n:]))
    disc = [d for d in p.dhcp_log[k:] if d["type"] == 1]
    t.check("[peer] and after a NAK no longer asks for the address refused",
            len(disc) >= 3 and all(50 not in d["opts"] for d in disc[1:]), str([d["opts"].get(50) for d in disc]))
    times = [d["time"] for d in disc]
    gaps = [b - a for a, b in zip(times, times[1:])]
    t.check("[peer] waiting longer each time", len(gaps) >= 2 and gaps[-1] > gaps[0] + 1, str(gaps))

    # Offers and acknowledgements that are not for us, then a good one.
    p.dhcp_mode = "evil"
    k = len(p.dhcp_log)
    out = plain(m.run("net dhcp", timeout=20))
    t.check("[peer] lies are passed over for the good offer", "Asking DHCP... 10.0.2.15/24, gateway 10.0.2.2" in out, out)
    req = [d for d in p.dhcp_log[k:] if d["type"] == 3]
    t.check("[peer] and only the good one is asked for", len(req) == 1 and req[0]["opts"].get(50) == GUEST_IP and
            req[0]["opts"].get(54) == GW_IP, str(req))
    p.dhcp_mode = "normal"

    # By hand.
    out = plain(m.run("net static 10.0.2.40/24 10.0.2.2 10.0.2.2"))
    k = len(p.dhcp_log)
    out = plain(m.run("ping -c 1 10.0.2.7", timeout=20))
    t.check("[peer] an address set by hand is used", "1 received" in out and p.ping_sources[-1:] == [bytes([10, 0, 2, 40])],
            out + str(p.ping_sources[-1:]))
    out = plain(m.run("host a.test"))
    t.check("[peer] with the DNS server given", "a.test has address 192.0.2.1\n" in out, out)
    out = plain(m.run("net"))
    t.check("[peer] and no time server", "Time:" not in out, out)
    n = len(p.ntp_queries)
    out = plain(m.run("sntp -q", timeout=30))
    t.check("[peer] so sntp falls back on pool.ntp.org", "sntp: pool.ntp.org" in out and "[exit status 1]" in out and
            len(p.ntp_queries) == n, out)
    time.sleep(1)
    t.check("[peer] and DHCP says nothing more", len(p.dhcp_log) == k, str(p.dhcp_log[k:]))
    out = plain(m.run("net dhcp", timeout=20))
    disc = [d for d in p.dhcp_log[k:] if d["type"] == 1]
    t.check("[peer] net dhcp asks for the last address DHCP gave, not the one typed",
            "Asking DHCP... 10.0.2.15/24" in out and disc and disc[0]["opts"].get(50) == GUEST_IP,
            out + str([d["opts"].get(50) for d in disc]))

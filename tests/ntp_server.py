"""An SNTP server for the tests. One server, many manners: the test sets
`mode` (and `offset` or `at`) before each run of sntp, and the server
answers every request that way until told otherwise. It keeps every
request it got in `requests`, as (bytes, time it arrived). A guest on
QEMU's user network reaches it as 10.0.2.2:PORT.

Modes:
  good        a proper reply, version 4, stratum 2
  v3          the same, version 3
  leap        the same, with a leap second announced (LI 1)
  forged      first a reply with the wrong origin timestamp, a day off,
              then the proper one
  kiss        a kiss-o'-death, RATE
  kiss-evil   a kiss-o'-death whose code is an escape sequence
  unsync      leap indicator 3: the server does not know the time
  stratum16   stratum 16: likewise
  mode3       a reply in client mode (a reflection, not an answer)
  short       40 bytes
  zero        a transmit timestamp of 0
  silent      nothing at all
  late        nothing for the first request, then proper replies
  slow        a proper reply, half a second after the request came
"""
import socket
import struct
import threading
import time

EPOCH = 2208988800  # 1970 in seconds since 1900


def ntp_ts(t):
    """Unix time (a float) as a 64-bit NTP timestamp, wrapped to 32 bits of seconds."""
    s = int(t // 1)
    return ((s + EPOCH) % 2 ** 32) << 32 | int((t - s) * 2 ** 32)


class Server:
    def __init__(self):
        self.mode = "good"
        self.offset = 0.0    # seconds added to the true time
        self.at = None       # or a Unix time to report, running on from when it was set
        self.set_at = 0.0
        self.requests = []
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.1)
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def set(self, mode="good", offset=0.0, at=None):
        with self.lock:
            self.mode, self.offset, self.at, self.set_at = mode, offset, at, time.time()
            self.requests = []

    def now(self):
        if self.at is not None:
            return self.at + (time.time() - self.set_at)
        return time.time() + self.offset

    def reply(self, q, li=0, vn=4, mode=4, stratum=2, refid=b"\xc0\x00\x02\x01", origin=None, recv=None, xmit=None):
        recv = ntp_ts(self.now()) if recv is None else recv
        origin = q[40:48] if origin is None else origin
        head = struct.pack("!BBbb", li << 6 | vn << 3 | mode, stratum, 6, -20)
        body = struct.pack("!II", 0x100, 0x200) + refid + struct.pack("!Q", ntp_ts(self.now() - 30)) + origin
        xmit = ntp_ts(self.now()) if xmit is None else xmit
        return head + body + struct.pack("!QQ", recv, xmit)

    def answer(self, q):
        mode = self.mode
        if mode == "silent" or (mode == "late" and len(self.requests) == 1):
            return []
        if mode in ("good", "late"):
            return [self.reply(q)]
        if mode == "v3":
            return [self.reply(q, vn=3)]
        if mode == "leap":
            return [self.reply(q, li=1)]
        if mode == "forged":
            wrong = bytes(b ^ 0xFF for b in q[40:48])
            fake = ntp_ts(self.now() + 86400)
            return [self.reply(q, origin=wrong, recv=fake, xmit=fake), self.reply(q)]
        if mode == "kiss":
            return [self.reply(q, stratum=0, refid=b"RATE")]
        if mode == "kiss-evil":
            return [self.reply(q, stratum=0, refid=b"\x1b[2J")]
        if mode == "unsync":
            return [self.reply(q, li=3)]
        if mode == "stratum16":
            return [self.reply(q, stratum=16)]
        if mode == "mode3":
            return [self.reply(q, mode=3)]
        if mode == "short":
            return [self.reply(q)[:40]]
        if mode == "zero":
            return [self.reply(q, xmit=0)]
        if mode == "slow":
            recv = ntp_ts(self.now())
            time.sleep(0.5)
            return [self.reply(q, recv=recv)]
        raise ValueError(mode)

    def _serve(self):
        while self.running:
            try:
                q, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.requests.append((q, time.time()))
                out = self.answer(q) if len(q) >= 48 else []
            for d in out:
                self.sock.sendto(d, addr)

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()

#!/usr/bin/env python3
"""Screenshots for the README: boot the real ISO in QEMU, use it the way a
person would, and save what the screen shows as PNG files.

    python3 tools/screenshots.py                    # after make
    python3 tools/screenshots.py --out /some/dir

It borrows the test suite's machine (tests/qemu_test.py), which drives the
shell over the serial port and the desktop through QEMU's monitor, and its
network peer (tests/net_peer.py), a script that plays the DHCP server, the
DNS server, the time server and a couple of hosts to ping. So the network
in the boot picture is that peer, not the internet. The disk is copied
first; nothing on the build's image changes. Python standard library only.
"""
import argparse
import os
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
import net_peer  # noqa: E402
import qemu_test  # noqa: E402


def ppm_to_png(ppm, png):
    """QEMU's screendump is a binary PPM (P6); write the same pixels as PNG."""
    with open(ppm, "rb") as f:
        data = f.read()
    magic, w, h, maxval, rest = data.split(None, 4)
    assert magic == b"P6" and maxval == b"255", "unexpected screendump format"
    w, h = int(w), int(h)
    rows = b"".join(b"\0" + rest[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        c = kind + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))

    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))
    return w, h


class Shooter:
    def __init__(self, machine, out, work):
        self.m, self.out, self.work = machine, out, work

    def shot(self, name):
        time.sleep(0.5)  # let the last thing drawn land on the screen
        ppm = os.path.join(self.work, name + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.m.monitor_cmd(f"screendump {ppm}")
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 20:
                time.sleep(0.2)
                break
            time.sleep(0.1)
        w, h = ppm_to_png(ppm, os.path.join(self.out, name + ".png"))
        print(f"{name}.png  {w}x{h}")

    def key(self, *keys):
        for k in keys:
            self.m.monitor_cmd(f"sendkey {k}")
            time.sleep(0.05)

    def type(self, text):
        names = {" ": "spc", ".": "dot", "\n": "ret"}
        for ch in text:
            self.key(names.get(ch, ch.lower()) if not ch.isupper() else "shift-" + ch.lower())

    def mouse(self, x, y):
        """Move the pointer by x, y in screen pixels, in steps QEMU sends whole."""
        while x or y:
            dx, dy = max(-100, min(100, x)), max(-100, min(100, y))
            self.m.monitor_cmd(f"mouse_move {dx} {dy}")
            x, y = x - dx, y - dy
            time.sleep(0.05)

    def click(self):
        self.m.monitor_cmd("mouse_button 1")
        time.sleep(0.15)
        self.m.monitor_cmd("mouse_button 0")
        time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", default="build/x86_64/ringtail.iso")
    ap.add_argument("--disk", default="build/x86_64/disk.img")
    ap.add_argument("--kernel", default="build/x86_64/ringtail.bin", help="the kernel's ELF file, for its symbols")
    ap.add_argument("--out", default="docs")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    nm = subprocess.run(["nm", args.kernel], capture_output=True, text=True, check=True).stdout
    qemu_test.Machine.CELLS = int(re.search(r"^([0-9a-f]+) [BbDd] console_cells$", nm, re.M).group(1), 16)

    work = tempfile.mkdtemp(prefix="ringtail-shots-")
    disk = os.path.join(work, "disk.img")
    shutil.copy(args.disk, disk)
    peer = net_peer.Peer()
    m = qemu_test.Machine(args.iso, disk, work, net=peer.qemu_args())
    s = Shooter(m, args.out, work)
    try:
        m.expect(qemu_test.PROMPT, 60)

        # The boot, and a few commands.
        m.run("run hello")
        m.run("date")
        m.run("ping -c 1 gw.lan")
        m.run("tasks")
        s.shot("boot")

        # The monitor.
        m.send("run gauge\r")
        time.sleep(3)
        s.shot("gauge")
        m.send("q")
        m.expect(qemu_test.PROMPT, 10)

        # The editor, on the welcome file that comes on the disk.
        m.send("run octavo WELCOME.TXT\r")
        m.wait_row(m.rows - 1, "Octavo", 20)  # the whole screen takes a few seconds over serial
        time.sleep(1)
        s.shot("octavo")
        m.send("\x11")  # Ctrl+Q
        m.expect(qemu_test.PROMPT, 10)

        # The desktop, with its About window open: About is the first
        # icon, top left.
        m.send("run kenoma\r")
        qemu_test.kenoma_up(m)
        time.sleep(1)
        qemu_test.kenoma_click(m, 76, 90)
        s.mouse(900, 330)  # beside the window, over no icon
        time.sleep(1)
        s.shot("desktop")
        s.key("esc")
        m.expect(qemu_test.PROMPT, 10)
    finally:
        peer.close()
        m.close()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""RingTail integration tests: boot the real ISO in QEMU and drive the
shell over the serial port, the way a person at a terminal would.

    python3 tests/qemu_test.py --iso build/ringtail.iso --disk build/disk.img

The disk image is copied first, so the build's image is never
modified. After the session the copy is checked with fsck.fat (when
installed), which proves the kernel left a consistent FAT16 volume
that other systems can read.
"""
import argparse
import datetime
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import dns_server
import ntp_server
import http.server
import ssl
import threading

# The prompt carries the current directory: "ringtail:/> ", "ringtail:/My Docs> ".
PROMPT = rb"ringtail:/[^>\n]*> "


class Machine:
    # net: QEMU arguments for the network card; by default QEMU gives an
    # e1000 on its user network, where 10.0.2.2 is the gateway.
    def __init__(self, iso, disk, workdir, cpu=None, mem=64, machine=None, net=()):
        self.dir = workdir
        self.serial_path = os.path.join(workdir, "serial.sock")
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-cdrom", iso,
             "-drive", f"file={disk},format=raw,if=ide,index=0",
             "-boot", "d", "-m", str(mem), "-display", "none",
             *(["-cpu", cpu] if cpu else []), *(["-machine", machine] if machine else []),
             *net,
             "-chardev", f"socket,id=ser,path={self.serial_path},server=on,wait=on",
             "-serial", "chardev:ser",
             "-monitor", f"unix:{self.monitor_path},server=on,wait=off",
             "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.serial = self._connect(self.serial_path)
        self.monitor = self._connect(self.monitor_path)
        self.buf = b""
        self.log = b""

    def _connect(self, path):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("QEMU did not start: " + self.qemu.stderr.read().decode(errors="replace"))

    def _fill(self, timeout):
        self.serial.settimeout(timeout)
        try:
            data = self.serial.recv(65536)
        except socket.timeout:
            return True
        if not data:
            return False
        self.buf += data
        self.log += data
        return True

    def _pending(self):
        return bool(select.select([self.serial], [], [], 0)[0])

    def expect(self, pattern, timeout=10):
        """Wait for a regex (bytes) in the output; return everything up to
        and including the match, consuming it."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return out.decode(errors="replace")
            left = end - time.time()
            if left <= 0 or not self._fill(min(left, 0.2)):
                if left <= 0:
                    raise TimeoutError(f"no {pattern!r} after {timeout}s; got: {self.buf[-600:]!r}")
                raise EOFError(f"serial closed waiting for {pattern!r}; got: {self.buf[-600:]!r}")

    def send(self, data):
        self.serial.sendall(data.encode() if isinstance(data, str) else data)

    def run(self, line, timeout=15):
        """Type a command and return its output (without the echo and prompt)."""
        self.send(line + "\r")
        out = self.expect(PROMPT, timeout)
        out = re.sub(PROMPT.decode() + "$", "", out).replace("\r\n", "\n")
        first_nl = out.find("\n")
        return out[first_nl + 1:] if first_nl >= 0 else ""

    def edit(self, keys):
        """Send keys without Enter and return what the line then looks
        like and where the cursor is, by replaying the bytes the shell
        wrote onto a one-line screen (backspace moves left, characters
        overwrite)."""
        def settle():
            quiet = 0
            while quiet < 3:
                before = len(self.buf)
                self._fill(0.1)
                quiet = quiet + 1 if len(self.buf) == before else 0
        settle()  # the tail of the last prompt
        self.buf = b""
        self.send(keys)
        settle()
        cells, col = [], 0
        for b in re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", self.buf):  # the prompt's colours
            if b == 0x08:
                col = max(col - 1, 0)
            elif 0x20 <= b < 0x7F:
                while len(cells) <= col:
                    cells.append(" ")
                cells[col] = chr(b)
                col += 1
        self.buf = b""
        return "".join(cells).rstrip(), col

    def monitor_cmd(self, cmd):
        """Run a monitor command and return what the monitor printed,
        reading until its next "(qemu)" prompt."""
        # Keep the serial port drained, or the guest stalls writing to it.
        while self._pending() and self._fill(0.01):
            pass
        self.monitor.settimeout(0.05)
        try:
            while self.monitor.recv(65536):  # whatever is left over
                pass
        except socket.timeout:
            pass
        self.monitor.sendall((cmd + "\n").encode())
        out = b""
        end = time.time() + 5
        self.monitor.settimeout(0.2)
        while time.time() < end:
            try:
                data = self.monitor.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            out += data
            # The echo of the command comes first, then output, then the prompt.
            tail = out.split(b"\n", 1)
            if len(tail) == 2 and b"(qemu)" in tail[1]:
                break
        return out.decode(errors="replace")

    def screen_row(self, row):
        """One row of the VGA text screen as (text, attributes)."""
        out = self.monitor_cmd(f"xp /80hx {0xb8000 + row * 160:#x}")
        cells = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{4})\b", out.split("\n", 1)[-1])][:80]
        return "".join(chr(c & 0xFF) for c in cells), [c >> 8 for c in cells]

    def screen_size(self):
        path = os.path.join(self.dir, "screen.ppm")
        if os.path.exists(path):
            os.remove(path)
        self.monitor_cmd(f"screendump {path}")
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 20:
                break
            time.sleep(0.1)
        with open(path, "rb") as f:
            header = f.read(20).split()
        return int(header[1]), int(header[2])

    def wait_row(self, row, text, timeout=5):
        """Wait until a screen row contains text; return the row."""
        end = time.time() + timeout
        while True:
            line, _ = self.screen_row(row)
            if text in line or time.time() > end:
                return line
            time.sleep(0.1)

    def close(self):
        try:
            self.qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.qemu.kill()
            self.qemu.wait()


class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            print(f"  ok    {name}")
        else:
            self.failed.append(name)
            print(f"  FAIL  {name}")
            if detail:
                print("        " + detail.strip().replace("\n", "\n        ")[:1500])


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)


HOST_FILE = False  # whether mtools put a long-named file on the disk


def plain(out):
    """Output without colour codes, carriage returns or surrounding whitespace."""
    return re.sub(r"\x1b\[[0-9;]*m", "", out).replace("\r\n", "\n").strip()


def tests(m, t):
    boot = m.expect(PROMPT, 30).replace("\r\n", "\n")
    t.check("boots to the shell", "RingTail OS v0.28.0" in boot, boot)
    t.check("finds the FAT16 disk", re.search(r"Disk: 16 MB FAT16, \d+ files", boot), boot)
    # The release's name is in the README's "Current version" line; the
    # banner must say the same (it said an old one for four releases).
    readme = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "README.md")).read()
    cur = re.search(r'Current version: \*\*(v[\d.]+) "([^"]+)"\*\*', readme)
    t.check("the banner names the release the README says is current",
            cur and f'RingTail OS {cur.group(1)} "{cur.group(2)}"' in boot, f"{cur and cur.groups()}: {boot}")

    out = m.run("help")
    t.check("help lists commands", "run <prog>" in out and "poweroff" in out, out)
    out = m.run("ls")
    t.check("ls shows programs and files", "HELLO" in out and "WELCOME.TXT" in out and "KB free" in out, out)
    out = m.run("cat welcome.txt")
    t.check("cat reads a file", "Welcome to RingTail." in out, out)
    out = m.run("frobnicate")
    t.check("unknown command is reported", "Unknown command" in out, out)

    out = m.run("run hello")
    t.check("run hello", "Hello from disk!" in out, out)
    out = m.run("run args one two three")
    t.check("arguments reach the program", "argc=4" in out and "argv[3]=three" in out, out)
    out = m.run("hello")
    t.check("a bare program name runs it", "Hello from disk!" in out, out)
    start = time.time()
    out = m.run("sleep 1")
    took = time.time() - start
    t.check("sleep 1 takes about a second", 0.8 < took < 3, "%.2f s: %s" % (took, out))
    out = m.run("sleep 99999")
    t.check("sleep rejects an hour and more", "usage" in out, out)
    out = m.run('echo "two   spaces"')
    t.check("echo keeps the spacing inside quotes", "two   spaces" in out, out)
    out = m.run("echo  two   spaces")
    t.check("and joins unquoted words with single spaces", "two spaces" in out and "two  " not in out, out)
    out = m.run("exit")
    t.check("exit gets a fresh shell from init", "starting a new one" in out, out)
    out = m.run("version")
    t.check("and the new shell works", "0.28.0" in out, out)

    for prog in ["badptr", "kptr", "filedemo", "seekdemo", "listdemo", "bss", "stack", "heaptest", "pipetest", "fgspawn",
                 "udptest", "tcptest", "rndtest"]:
        out = m.run("run " + prog, timeout=30)
        t.check(f"{prog} self-checks pass", f"{prog}: all" in out and "FAIL" not in out, out)
    out = m.run("run nullds")
    t.check("null data segments cannot hurt a syscall", "worked" in out, out)

    # The network, through QEMU's user network: 10.0.2.2 is its gateway
    # and DHCP server, and answers ARP and pings; 10.0.2.3 is its DNS
    # server; nobody else on 10.0.2.0/24 answers.
    t.check("finds the network card", "Network: e1000 82540EM 52:54:00:12:34:56\n" in boot, boot)
    t.check("and gets an address from DHCP before the shell starts",
            "Network: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 (DHCP)\nClock: " in boot, boot)
    # QEMU's DHCP names no time server, so the boot sync asks the pool,
    # which may or may not answer from here: any of its three endings will do.
    clock = re.search(r"\nClock: ([^\n]*)\nType 'help'", boot)
    t.check("then the clock is set from pool.ntp.org, or said not to be, in one line",
            clock and "pool.ntp.org" in clock.group(1) and re.match(
                r"(set to \d{4}-\d\d-\d\d \d\d:\d\d:\d\d by |\d{4}-\d\d-\d\d \d\d:\d\d:\d\d, "
                r"(right to within half a second by |as the CMOS has it \())", clock.group(1)), boot)
    out = m.run("pci")
    t.check("pci lists the bus", re.search(r"00:03\.0  8086:100e  Intel ethernet +irq 11", out) and
            "Intel IDE controller" in out, out)
    t.check("with nothing trailing off its lines", all(l == l.rstrip() for l in plain(out).splitlines()), repr(out))
    mem_before = m.run("mem")
    out = m.run("ping -c 3 10.0.2.2", timeout=30)
    t.check("ping 10.0.2.2 gets three replies", out.count("bytes from 10.0.2.2") == 3 and
            "3 sent, 3 received, 0% lost, round trip min/avg/max" in out and "[exit status" not in out, out)
    t.check("each with its ttl and round trip", re.search(r"64 bytes from 10\.0\.2\.2: seq=1 ttl=\d+ time=\d+\.\d{3} ms", out), out)
    out = m.run("ping -c 1 -s 1472 10.0.2.2")
    t.check("the biggest ping that fits a frame", "1480 bytes from 10.0.2.2" in out, out)
    out = m.run("ping -c 1 -s 0 10.0.2.2")
    t.check("and an empty one", "8 bytes from 10.0.2.2" in out, out)
    out = plain(m.run("net"))
    t.check("net shows the card", "e1000 82540EM (8086:100e) at 00:03.0, irq 11" in out and "link up" in out, out)
    t.check("the address", "Address:  10.0.2.15/24, gateway 10.0.2.2\n" in out, out)
    t.check("where it came from and for how long", re.search(r"DHCP:     from 10\.0\.2\.2, lease 2[34] h \d+ m left", out), out)
    t.check("and the DNS server", "DNS:      10.0.2.3\n" in out, out)
    t.check("the traffic", (c := re.search(r"Frames:\s+(\d+) in, (\d+) out", out)) and
            int(c.group(1)) >= 5 and int(c.group(2)) >= 5, out)
    t.check("and the gateway's MAC, learned by ARP", "10.0.2.2 is 52:55:0a:00:02:02" in out, out)
    start = time.time()
    out = m.run("ping -c 1 10.0.2.99", timeout=20)
    took = time.time() - start
    t.check("a neighbour nobody answers for is host unreachable", "seq=1: host unreachable" in out and
            "1 sent, 0 received, 100% lost" in out and "[exit status 1]" in out, out)
    t.check("after three ARP requests a second apart", 2.5 < took < 6, "%.2f s" % took)
    out = m.run("ping -c 2 127.0.0.1")
    t.check("loopback answers", "2 sent, 2 received" in out and "64 bytes from 127.0.0.1: seq=2 ttl=64" in out, out)
    out = m.run("ping -c 1 127.9.9.9")
    t.check("all of 127/8 is loopback", "1 received" in out, out)
    out = m.run("ping -c 1 10.0.2.15")
    t.check("and so is our own address", "1 received" in out, out)
    out = m.run("ping -c 1 localhost")
    t.check("localhost is 127.0.0.1, with nobody asked",
            "PING localhost (127.0.0.1): 56 data bytes" in out and "1 received" in out, out)
    out = m.run("net")
    t.check("net counts the pings answered and the packets looped",
            re.search(r"Pings:    [4-9] answered; \d+ packets through loopback", out), out)
    for bad in ["", "-c", "-s 1473 10.0.2.2", "-W 0 10.0.2.2", "-x 10.0.2.2", "10.0.2.2 10.0.2.3"]:
        out = m.run("ping " + bad)
        t.check(f"ping {bad or '(nothing)'} is a usage error", "usage: ping" in out and "[exit status 2]" in out, out)
    for bad in ["10.0.2", "10.0.2.256", "10.0.2.2.", "1..2.3", "bad_name..com", "a" * 64 + ".com"]:
        out = m.run("ping " + bad)
        t.check(f"{bad[:20]} is neither an address nor a name", f"ping: {bad}: not a valid name" in out and
                "[exit status 2]" in out, out)
    out = m.run("ping 224.0.0.1")
    t.check("multicast cannot be pinged", "ping: invalid argument" in out, out)
    m.send("ping -c 0 10.0.2.2\r")
    m.expect(rb"seq=2 ", 10)
    m.send("\x03")
    m.expect(PROMPT, 5)
    out = m.run("echo after ping")
    t.check("Ctrl+C stops an endless ping", "after ping" in out, out)
    t.check("pings leave memory as they found it", m.run("mem") == mem_before, mem_before)

    dns_tests(m, t)
    ntp_tests(m, t)
    tcp_tests(m, t)
    text_tests(m, t)
    netconf_tests(m, t)

    # The fuzzer throws hostile arguments at every non-blocking system call.
    # The kernel must come through with its heap and its frames intact.
    mem0 = m.run("mem")
    for seed in (1, 2, 3):
        out = m.run(f"run fuzz {seed} 3000", timeout=120)
        t.check(f"syscall fuzzing, seed {seed}", "fuzz: all" in out and "KERNEL PANIC" not in out and "FAIL" not in plain(out), out[-400:])
    t.check("the fuzzer left no frames or heap behind", m.run("mem") == mem0, mem0 + m.run("mem"))
    out = m.run("ls /")
    t.check("and its scratch directory is gone", "FZ" not in out, out)

    out = m.run("run crash")
    t.check("privileged instruction kills only the program",
            "general protection" in out.lower() and "task killed" in out, out)
    out = m.run("run fault")
    t.check("null dereference kills only the program", "page fault" in out.lower() and "task killed" in out, out)
    out = m.run("run stack overflow", timeout=30)
    t.check("runaway recursion is stopped at the stack limit", "task killed" in out, out)
    out = m.run("run gfxfault")
    t.check("a fault in graphics mode is reported", "task killed" in out, out)
    t.check("and the screen is back in text mode", m.screen_size() == (720, 400))
    out = m.run("run hello")
    t.check("programs still run after all that", "Hello from disk!" in out, out)
    out = m.run("faults")
    t.check("faults were logged", out.count("): ") >= 4, out)

    # Ctrl+C stops the foreground program.
    m.send("run count 30\r")
    m.expect(rb"count: 2", 10)
    start = time.time()
    m.send("\x03")
    out = m.expect(PROMPT, 5)
    t.check("Ctrl+C stops a running program", "^C" in out and time.time() - start < 2, out)

    # Pipes and redirection.
    out = m.run("hello | wc")
    t.check("a program pipes into another", "      1       3      17" in out, out)
    out = m.run("ls | grep -i txt")
    t.check("ls pipes into a program", "WELCOME.TXT" in out and "HELLO" not in out, out)
    t.check("and leaves out what is only for the screen", "files," not in out and "\x1b[90m" not in out, out)
    out = m.run("help | grep poweroff")
    t.check("help can be searched", "poweroff" in out and "history" not in out, out)
    m.run('echo "one  two" > out.txt')
    m.run("echo three >> out.txt")
    out = m.run("cat out.txt")
    t.check("> writes a file and >> appends", "one  two\nthree" in out, out)
    out = m.run("wc < out.txt")
    t.check("< feeds a file to a program", "      2       3      15" in out, out)
    out = m.run("grep three < out.txt | wc > count.txt")
    out = m.run("cat count.txt")
    t.check("a pipeline with both redirections", "      1       1       6" in out, out)
    out = m.run("hello > out.txt")
    out = m.run("cat out.txt")
    t.check("> replaces what was there", "Hello from disk!" in out and "three" not in out, out)
    out = m.run("hello | frobnicate")
    t.check("an unknown command in a pipeline is reported", "Unknown command 'frobnicate'" in out, out)
    out = m.run("hello |")
    t.check("a dangling | is an error", "Missing command" in out, out)
    out = m.run("hello | pwd")
    t.check("a built-in cannot come after |", "built into the shell" in out, out)
    out = m.run("hello >")
    t.check("> needs a file name", "Missing file name" in out, out)
    out = m.run("pwd < out.txt")
    t.check("built-ins do not take <", "do not read input" in out, out)
    out = m.run("cat < out.txt")
    t.check("cat is a program, so it takes <", "Hello from disk!" in out, out)
    out = m.run("wc < nosuch.txt")
    t.check("a missing input file is reported", "no such file" in out, out)
    m.send("sleep 30 | wc\r")
    time.sleep(1)
    start = time.time()
    m.send("\x03")
    out = m.expect(PROMPT, 5)
    t.check("Ctrl+C stops the whole pipeline", "^C" in out and time.time() - start < 2, out)
    out = m.run("tasks")
    t.check("and leaves nothing behind", "sleep" not in out and "wc" not in out, out)
    m.run("rm out.txt")
    m.run("rm count.txt")

    # Background job, then the task list.
    out = m.run("run count 3 &")
    t.check("a background job starts", "started in the background" in out, out)
    out = m.run("tasks")
    t.check("tasks lists it", "count" in out and "user" in out, out)
    m.expect(rb"count done", 10)

    # A program that never yields cannot freeze the shell.
    out = m.run("run spin &")
    busy = re.search(r"\[(\d+)\] started", out)
    t.check("a spinning program starts in the background", busy, out)
    start = time.time()
    out = m.run("uptime")
    t.check("the shell stays responsive next to it", "Up " in out and time.time() - start < 3, out)
    if busy:
        out = m.run("kill " + busy.group(1))
        t.check("kill stops it", "Killed task" in out, out)
    out = m.run("kill 1")
    t.check("kernel tasks cannot be killed", "not permitted" in out, out)
    out = m.run("tasks")
    shell = re.search(r"^\s*(\d+)\s+\d+\s+user\s+\S+\s+\S+\s+saklas\s*$", out, re.M)
    t.check("the shell is a user task", shell, out)
    if shell:
        out = m.run("kill " + shell.group(1))
        t.check("the shell refuses to kill itself", "will not kill itself" in out, out)

    # Interactive stdin.
    m.send("run readline\r")
    m.expect(rb"> ", 5)
    m.send("abc def\r")
    out = m.expect(rb"got .*\n", 5)
    t.check("a program reads a line from the terminal", 'got "abc def"' in out, out)
    m.expect(rb"> ", 5)
    m.send("\x04")
    out = m.expect(PROMPT, 5)
    t.check("Ctrl+D is end of file", "readline: 1 lines" in out, out)

    # Mouse input wakes a waiting program.
    m.send("run waitdemo\r")
    m.expect(rb"Waiting for input", 5)
    time.sleep(0.3)
    m.monitor_cmd("mouse_move 5 5")
    out = m.expect(PROMPT, 5)
    t.check("mouse movement wakes a waiting program", "Woke up on input." in out, out)

    # Timer: 100 ticks per second of wall time.
    a = int(re.search(r"(\d+) ticks", m.run("uptime")).group(1))
    t0 = time.time()
    time.sleep(3)
    b = int(re.search(r"(\d+) ticks", m.run("uptime")).group(1))
    rate = (b - a) / (time.time() - t0)
    t.check(f"timer runs at 100 Hz (measured {rate:.0f})", 80 <= rate <= 120)

    # Shell file commands.
    m.run("write note.txt hello   world")
    out = m.run("cat NOTE.TXT")
    t.check("write then cat", "hello   world" in out, out)
    out = m.run("touch note.txt")
    t.check("touch of an existing file is fine and leaves it alone", plain(out) == "" and "hello   world" in m.run("cat note.txt"), out)
    out = m.run("touch bad*name")
    t.check("bad names are rejected", "not a valid file name" in out, out)
    m.run("rm note.txt")
    out = m.run("cat note.txt")
    t.check("rm deletes", "no such file" in out, out)
    m.run("write keep.txt this survives a power cycle")

    # Directories.
    out = m.run("mkdir docs")
    t.check("mkdir", plain(out) == "", out)
    out = m.run("mkdir docs")
    t.check("mkdir of a taken name is an error", "already exists" in out, out)
    out = m.run("ls")
    t.check("ls marks directories", re.search(r"docs/\s+<dir>", plain(out)) and "1 directory" in out, out)
    m.run("write docs/a.txt in a directory")
    out = m.run("cat DOCS/A.TXT")
    t.check("a file in a directory, by path", "in a directory" in out, out)
    m.send("cd docs\r")
    out = m.expect(PROMPT, 5)
    t.check("cd changes the prompt, with the name as written", "ringtail:/docs> " in out, out)
    out = m.run("pwd")
    t.check("pwd", plain(out) == "/docs", out)
    out = m.run("cat a.txt")
    t.check("relative paths start at the current directory", "in a directory" in out, out)
    out = m.run("cat ../keep.txt")
    t.check(".. is the parent", "survives" in out, out)
    out = m.run("cat /welcome.txt")
    t.check("a leading / starts at the root", "Welcome to RingTail." in out, out)
    out = m.run("hello")
    t.check("programs still come from / in a directory", "Hello from disk!" in out, out)
    out = m.run("run filedemo", timeout=30)
    t.check("a program works in the current directory", "filedemo: all" in out and "FAIL" not in out, out)
    m.run("mkdir sub")
    m.run("cd sub")
    out = m.run("pwd")
    t.check("nested directories", plain(out) == "/docs/sub", out)
    out = m.run("cd nothere")
    t.check("cd to a missing directory", "no such file" in out, out)
    out = m.run("cd /keep.txt")
    t.check("cd to a file", "not a directory" in out, out)
    out = m.run("cat /docs")
    t.check("cat of a directory", "is a directory" in out, out)
    out = m.run("rmdir /docs/sub")
    t.check("rmdir of the current directory is refused", "in use" in out, out)
    m.send("cd\r")
    out = m.expect(PROMPT, 5)
    t.check("cd alone goes to the root", out.rstrip().endswith("ringtail:/>"), out)
    out = m.run("rmdir docs")
    t.check("rmdir of a non-empty directory is refused", "not empty" in out, out)
    out = m.run("rm docs")
    t.check("rm of a directory points at rmdir", "rmdir" in out, out)
    out = m.run("rmdir docs/sub")
    t.check("rmdir", plain(out) == "", out)
    out = m.run("ls docs")
    t.check("ls of a directory", "a.txt" in out and "sub" not in out, out)

    # mv and cp.
    m.run("write m1.txt moving text")
    out = m.run("mv m1.txt m2.txt")
    t.check("mv renames", plain(out) == "" and "moving text" in m.run("cat m2.txt"), out)
    out = m.run("mv m2.txt docs")
    t.check("mv into a directory", "moving text" in m.run("cat docs/m2.txt"), out)
    out = m.run("mv -n docs/m2.txt docs/a.txt")
    t.check("mv -n leaves an existing file alone", plain(out) == "" and "moving text" in m.run("cat docs/m2.txt"), out)
    m.run("mkdir docs/in")
    out = m.run("mv docs docs/in")
    t.check("mv of a directory into itself is refused", "inside itself" in out, out)
    m.run("mkdir moved")
    out = m.run("mv docs moved")
    t.check("mv moves a whole directory", "in a directory" in m.run("cat moved/docs/a.txt"), out)
    m.run("cd moved/docs/in")
    out = m.run("cat ../../../keep.txt")
    t.check("and .. inside it leads back up", "survives" in out, out)
    m.run("cd /")
    out = m.run("cp keep.txt copy.txt")
    t.check("cp copies a file", "survives" in m.run("cat copy.txt"), out)
    out = m.run("cp copy.txt moved/docs/in")
    t.check("cp into a directory", "survives" in m.run("cat moved/docs/in/copy.txt"), out)
    out = m.run("cp hello moved")
    t.check("cp copies a program", "Hello from disk!" in m.run("moved/hello"), out)
    out = m.run("cp copy.txt copy.txt")
    t.check("cp onto itself is refused", "same file" in out and "survives" in m.run("cat copy.txt"), out)
    out = m.run("cp moved x")
    t.check("cp of a directory is refused", "is a directory" in out, out)
    out = m.run("cp a b c")
    t.check("cp of several needs a directory", "directory last" in out, out)
    m.run("write copy.txt replaced")
    m.run("cp copy.txt moved/docs/in/copy.txt")
    out = m.run("cat moved/docs/in/copy.txt")
    t.check("cp replaces an existing file", "replaced" in out and "survives" not in out, out)

    m.run("mkdir keepdir")
    m.run("write keepdir/deep.txt nested and kept")

    # Long names.
    out = m.run('mkdir "My Docs"')
    t.check("mkdir of a name with a space", plain(out) == "", out)
    m.send('cd "my docs"\r')
    out = m.expect(PROMPT, 5)
    t.check("the prompt shows a long name as written", "ringtail:/My Docs> " in out, out)
    m.run('write "A rather long file name.text" long names work')
    out = m.run('cat "a RATHER long FILE name.TEXT"')
    t.check("a long name in any case", "long names work" in out, out)
    out = m.run("cat arathe~1.tex")
    t.check("and by its short alias", "long names work" in out, out)
    out = m.run("ls")
    t.check("ls shows a long name whole", re.search(r"A rather long file name\.text\s+\d+", plain(out)), out)
    out = m.run('mv "A rather long file name.text" Short.txt')
    t.check("mv from a long name to a short one", "long names work" in m.run("cat short.txt"), out)
    out = m.run('mv short.txt "Back to a long one.txt"')
    t.check("and back", "long names work" in m.run('cat "back to a long one.txt"'), out)
    out = m.run('mv "back to a long one.txt" "Back To A Long One.txt"')
    t.check("mv that changes only case", "Back To A Long One.txt" in m.run("ls"), out)
    out = m.run('mv "Back To A Long One.txt" "a:b"')
    t.check("mv to a bad name says so", "not a valid file name" in out, out)
    out = m.run("touch name.")
    t.check("a name ending in a dot is refused", "not a valid file name" in out, out)
    if HOST_FILE:
        out = m.run('cat "/From the host, with love.txt"')
        t.check("a long name another system wrote", "written by mtools" in out, out)
    m.run("cd /")

    # Tab completion.
    m.run('mkdir "Tab Test"')
    m.run('mkdir "Tab Test/Sub Dir"')
    for f in ("Alpha One.txt", "alpha two.txt", "Beta.txt"):
        m.run('touch "Tab Test/%s"' % f)
    out = plain(m.run("echo wel\t"))
    t.check("Tab completes a file, in the disk's case", out.strip() == "WELCOME.TXT", out)
    out = plain(m.run("echo tab\tbeta\t"))
    t.check("Tab quotes a name with a blank, through a directory", out.strip() == "Tab Test/Beta.txt", out)
    out = plain(m.run("hell\t"))
    t.check("Tab completes a program name in lower case", "Hello from disk!" in out, out)
    out = plain(m.run("ver\t"))
    t.check("Tab completes a built-in", "RingTail OS v0.28.0" in out, out)
    out = plain(m.run("run arg\t a b"))
    t.check("Tab completes the program after run", "argc=3" in out, out)
    m.run('cd "Tab Test"')
    out = plain(m.run("echo al\tt\t"))
    t.check("Tab extends to the common part, then finishes", out.strip() == "alpha two.txt", out)
    out = plain(m.run("cd s\t\""))
    t.check("Tab on cd finds a directory", "no such" not in out, out)
    out = m.run("pwd")
    t.check("and cd went there", plain(out).strip() == "/Tab Test/Sub Dir", out)
    m.run("cd ..")
    out = plain(m.run("cd b\t"))
    t.check("Tab on cd skips files", "Beta.txt" not in out and "b: no such file" in out, out)
    m.run("echo hi > bet\t")
    t.check("Tab after > completes the target", "hi" in m.run("cat beta.txt"))
    m.send("echo \t")
    out = plain(m.expect(PROMPT, 5))
    m.send("\x15")  # Ctrl+U: drop the line
    m.send("\r")
    m.expect(PROMPT, 5)
    t.check("Tab with several matches lists them", all(n in out for n in ("Alpha One.txt", "alpha two.txt", "Beta.txt", "Sub Dir/")), out)
    m.run("cd /")
    m.run('rmdir "Tab Test/Sub Dir"')
    for f in ("Alpha One.txt", "alpha two.txt", "Beta.txt"):
        m.run('rm "Tab Test/%s"' % f)
    m.run('rmdir "Tab Test"')

    # Editing in the middle of a line. "echo " is five characters.
    LEFT, RIGHT, HOME, END, DEL = "\x1b[D", "\x1b[C", "\x1b[H", "\x1b[F", "\x1b[3~"
    CLEFT, CRIGHT = "\x1b[1;5D", "\x1b[1;5C"
    cases = [
        ("insert in the middle", "echo helo" + LEFT + "l", "echo hello", 9, "hello"),
        ("Home and Right", "echo b" + HOME + RIGHT * 5 + "a", "echo ab", 6, "ab"),
        ("Delete", "echo abc" + LEFT * 2 + DEL, "echo ac", 6, "ac"),
        ("Backspace mid-line", "echo abc" + LEFT + "\x7f", "echo ac", 6, "ac"),
        ("End", "echo abc" + HOME + END + "d", "echo abcd", 9, "abcd"),
        ("Delete at the end does nothing", "echo ab" + DEL, "echo ab", 7, "ab"),
        ("Left at the start stops", "echo a" + HOME + LEFT * 3 + "x", "xecho a", 1, None),
        ("Ctrl+Left by word", "echo one two" + CLEFT * 2 + "x", "echo xone two", 6, "xone two"),
        ("Ctrl+Right by word", "echo one two" + HOME + CRIGHT * 2 + "x", "echo one xtwo", 10, "one xtwo"),
        ("Ctrl+K kills to the end", "echo abc def" + CLEFT + "\x0b", "echo abc", 9, "abc"),
        ("Ctrl+W mid-line", "echo abc def" + CLEFT + "\x17", "echo def", 5, "def"),
        ("Ctrl+A and Ctrl+E", "echo abc\x01\x05d", "echo abcd", 9, "abcd"),
    ]
    for name, keys, shown, col, result in cases:
        text, c = m.edit(keys)
        t.check("line editor, %s: display" % name, text == shown and c == col, "%r col %d" % (text, c))
        m.send("\r")
        out = plain(m.expect(PROMPT, 5))
        if result is not None:
            t.check("line editor, %s: result" % name, result in out.replace("\r", "").split("\n"), out)

    # A line longer than the screen is wide, edited across the wrap, as
    # the VGA memory shows it.
    long = "echo " + "a" * 85
    m.edit(long + LEFT * 20 + "Z" + DEL + DEL)
    rows = [m.screen_row(r)[0] for r in range(25)]
    start = next((i for i in reversed(range(25)) if rows[i].startswith("ringtail:/> echo")), None)
    shown = "".join(rows[start:start + 2]).rstrip() if start is not None else ""
    want = "ringtail:/> echo " + "a" * 65 + "Z" + "a" * 18
    t.check("editing across a screen wrap redraws correctly", shown == want, shown)
    m.send("\r")
    out = plain(m.expect(PROMPT, 5))
    t.check("and the wrapped line runs as edited", "a" * 65 + "Z" + "a" * 18 in out, out)

    # The clock and file timestamps. QEMU's RTC starts on the host's UTC.
    out = plain(m.run("date"))
    mt = re.match(r"(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)", out.strip())
    guest = datetime.datetime(*map(int, mt.groups())) if mt else None
    skew = abs((utcnow() - guest).total_seconds()) if guest else 1e9
    t.check("date shows the host's clock", skew < 120, "%s (skew %s s)" % (out, skew))
    m.run("write stamped.txt now")
    out = plain(m.run("ls"))
    row = re.search(r"STAMPED\.TXT|stamped\.txt", out) and re.search(r"stamped\.txt\s+\d+\s+(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d)", out)
    when = datetime.datetime(*map(int, row.groups())) if row else None
    skew = abs((utcnow() - when).total_seconds()) if when else 1e9
    t.check("a new file's ls line has the time it was written", skew < 180, out)
    t.check("files on the image carry the build time", re.search(r"HELLO\s+\d+\s+20\d\d-\d\d-\d\d ", out), out)
    m.run("rm stamped.txt")

    # Raw mode: keys arrive as bytes, arrows as xterm sequences.
    m.send("run rawkeys\r")
    m.expect(rb"raw mode on", 5)
    m.send("a")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("raw mode delivers a key at once, unechoed", "keys: 61" in out, out)
    m.send("\x1b[A")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("Up arrives as ESC [ A", "keys: 1b 5b 41" in out, out)
    m.send("\x1b[1;5D")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("Ctrl+Left arrives as ESC [ 1 ; 5 D", "keys: 1b 5b 31 3b 35 44" in out, out)
    m.send("\x7f")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("Backspace arrives as DEL", "keys: 7f" in out, out)
    m.send("\x03")
    out = m.expect(PROMPT, 5)
    t.check("Ctrl+C still kills a raw program", "^C" in out, out)
    out = m.run("echo cooked")
    t.check("the terminal is cooked again afterwards", "cooked" in out, out)

    m.send("run rawkeys nosig\r")
    m.expect(rb"raw mode on", 5)
    m.send("\x03")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("with TTY_NOSIG, Ctrl+C is a byte", "keys: 03" in out, out)
    m.send("\x03\x03\x03")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("three quick Ctrl+C are just bytes too", "03" in out and "^C" not in out, out)
    m.send("\x1c")
    out = m.expect(PROMPT, 5)
    t.check("Ctrl+\\ kills a TTY_NOSIG program", "^C" in out, out)

    # ANSI escapes position and colour text on the VGA screen.
    m.run("run ansi")
    row4, attr4 = m.screen_row(4)
    t.check("ESC [ row ; col H positions the cursor", row4[9:16] == "AT-5-10", repr(row4))
    t.check("SGR sets colours", row4[16:19] == "RED" and attr4[16] == 0x14 and attr4[19] != 0x14,
            f"{row4!r} {attr4[15:20]}")
    row6, _ = m.screen_row(6)
    t.check("cursor back and erase to end of line", row6.rstrip() == "abcX", repr(row6))
    row7, _ = m.screen_row(7)
    t.check("save and restore the cursor", row7.rstrip() == "!AVE", repr(row7))

    # Octavo: type, save, undo, replace, quit.
    m.send("run octavo notes.txt\r")
    t.check("octavo starts on a new file", "notes.txt" in m.wait_row(24, "notes.txt"))
    m.send("hello world\rsecond line")
    row0 = m.wait_row(0, "hello world")
    t.check("octavo shows typed text", row0.rstrip() == "hello world", repr(row0))
    t.check("and the caret readout follows", "Ln 2, Col 12" in m.wait_row(24, "Ln 2, Col 12"))
    m.send("\x13")
    t.check("Ctrl+S saves", "Saved notes.txt" in m.wait_row(23, "Saved"))
    m.send("\x1b[1;5F extra")  # Ctrl+End, then type
    m.wait_row(1, "extra")
    m.send("\x1a")  # Ctrl+Z undoes the whole typed run
    row1 = m.wait_row(1, "second line")
    for _ in range(20):
        if "extra" not in row1:
            break
        time.sleep(0.1)
        row1, _ = m.screen_row(1)
    t.check("Ctrl+Z undoes a typed run in one step", row1.rstrip() == "second line", repr(row1))
    m.send("\x12world\rthere\r")  # Ctrl+R: find, replace with
    m.wait_row(23, "Replace this one")
    m.send("a")
    t.check("replace all reports the count", "1 occurrence matched." in m.wait_row(23, "occurrence"))
    m.send("\x01\x03\x1b[1;5F\r\x16")  # select all, copy, Ctrl+End, Enter, paste
    row3 = m.wait_row(3, "second line")
    row2, _ = m.screen_row(2)
    t.check("copy and paste", row2.rstrip() == "hello there" and row3.rstrip() == "second line",
            repr(row2 + row3))
    m.send("\x11")
    t.check("quitting with changes asks first", "Save changes before closing" in m.wait_row(23, "Save changes"))
    m.send("y")
    out = m.expect(PROMPT, 5)
    out = m.run("cat notes.txt")
    t.check("the saved file has every edit", out.replace("\r", "") ==
            "hello there\nsecond line\nhello there\nsecond line\n" or
            "hello there\nsecond line\nhello there\nsecond line" in out, repr(out))

    m.send("run octavo\r")
    m.wait_row(24, "Untitled")
    m.send("lemur " * 20)
    row1 = m.wait_row(1, "lemur")
    t.check("long lines wrap at a word", m.screen_row(0)[0].rstrip().endswith("lemur") and
            row1.startswith("lemur"), repr(row1))
    m.send("\x13")
    m.wait_row(23, "Save as")
    m.send("zoo\r")
    t.check("an untitled document is saved as a lemur", "Saved zoo.lemur" in m.wait_row(23, "Saved"))
    m.send("\x11")
    m.expect(PROMPT, 5)
    out = m.run("cat ZOO.LEMUR")
    t.check("and the file is there", out.count("lemur") == 20, repr(out))

    # History recall with the Up arrow.
    m.run("echo first")
    m.send("\x1b[A\r")
    out = m.expect(PROMPT, 5)
    t.check("Up arrow recalls the last command", out.count("first") >= 2, out)

    # SYS_POLL: wait for terminal input, but only so long.
    m.send("run polldemo\r")
    out = m.expect(rb"ready", 5)
    mm = re.search(r"timeout: (\d+) after (\d+) ticks", out)
    t.check("a poll with nothing waiting times out with 0", mm and mm.group(1) == "0", out)
    t.check("after about the time asked for", mm and 30 <= int(mm.group(2)) <= 45, out)
    t.check("a poll of zero ticks answers at once", "zero: 0" in out, out)
    time.sleep(0.5)
    m.send("x")
    out = m.expect(PROMPT, 5)
    t.check("a key ends the poll early with 1", "input: 1 key 78" in out, out)
    t.check("and the key is still there to read, then nothing waits", "again: 0" in out, out)
    m.send("run polldemo hang\r")
    m.expect(rb"polling", 5)
    m.send("\x1c")
    out = m.expect(PROMPT, 5)
    t.check("a program in a long poll can be killed", "poll ended" not in out, out)

    # Gauge, the system monitor. gauge -1 is one plain snapshot.
    out = m.run("gauge -1")
    t.check("gauge -1 shows the version and clock", "RingTail OS v0.28.0" in out and " up " in out, out)
    mem = re.search(r"Frames\s+\[([#.]{30})\]\s+(\d+)%\s+(\d+) KB used of (\d+) KB", out)
    t.check("gauge shows frame usage that adds up", mem and int(mem.group(3)) < int(mem.group(4)), out)
    t.check("and heap and disk usage", re.search(r"Heap\s+\[[#.]{30}\]\s+\d+%", out) and
            re.search(r"Disk\s+\[[#.]{30}\]\s+\d+%", out), out)
    t.check("and the task table", re.search(r"^\s*\d+\s+\d+\s+user\s+running\s+\d+K\s+gauge$", out, re.M) and
            re.search(r"^\s*\d+\s+\d+\s+kernel\s+\w+\s+-\s+idle$", out, re.M), out)
    t.check("with a count that matches the rows",
            (c := re.search(r"Tasks\s+(\d+):", out)) and
            len(re.findall(r"^\s*\d+\s+\d+\s+(?:user|kernel)\s", out, re.M)) == int(c.group(1)), out)
    out = m.run("gauge -1 | grep saklas")
    t.check("gauge -1 can be piped", len(plain(out).splitlines()) == 1 and "user" in out, out)
    out = m.run("gauge nonsense")
    t.check("gauge refuses what it does not understand", "usage: gauge" in out, out)

    m.send("gauge\r")
    first = m.wait_row(0, " Gauge ")
    t.check("gauge draws a title row", first.startswith(" Gauge  RingTail OS v0.28.0"), repr(first))
    title, attrs = m.screen_row(0)
    t.check("the title is black on grey across the row", set(attrs) == {0x70}, f"{attrs[:3]} .. {attrs[-3:]}")
    t.check("and the footer lists the keys", "q quit" in m.wait_row(24, "q quit"))
    bar, battr = m.screen_row(2)
    t.check("usage bars are drawn with block characters", "\xb0" in bar or "\xdb" in bar, repr(bar))
    t.check("memory and the tasks are on screen", "Frames" in bar and "gauge" in "".join(
        m.screen_row(r)[0] for r in range(8, 14)), repr(bar))

    def uptime_on_screen():
        mm = re.search(r"up (\d+):(\d\d):(\d\d)", m.screen_row(0)[0])
        return int(mm.group(1)) * 3600 + int(mm.group(2)) * 60 + int(mm.group(3)) if mm else -1

    def settle(seconds):
        end = time.time() + seconds
        while time.time() < end:
            m._fill(0.1)  # keep the serial port drained, as a terminal would

    before = uptime_on_screen()
    settle(2.5)
    after = uptime_on_screen()
    t.check("gauge refreshes by itself", after >= before + 2, f"{before} -> {after}")
    m.send("p")
    settle(0.5)
    t.check("p pauses it", "PAUSED" in m.screen_row(24)[0], repr(m.screen_row(24)[0]))
    frozen = uptime_on_screen()
    settle(2.5)
    t.check("a paused gauge stands still", uptime_on_screen() == frozen, f"{frozen} -> {uptime_on_screen()}")
    m.send("p")
    settle(1.5)
    t.check("and p starts it again", uptime_on_screen() > frozen and "PAUSED" not in m.screen_row(24)[0],
            f"{frozen} -> {uptime_on_screen()}")
    m.send("-")
    settle(0.5)
    t.check("- slows it down", "every 2 s" in m.screen_row(24)[0], repr(m.screen_row(24)[0]))
    m.send("++")
    settle(0.5)
    t.check("+ speeds it up", "every 0.5 s" in m.screen_row(24)[0], repr(m.screen_row(24)[0]))
    m.send("q")
    m.expect(PROMPT, 5)
    row0, _ = m.screen_row(0)
    t.check("q leaves gauge and clears the screen", "Gauge" not in row0 and "q quit" not in m.screen_row(24)[0], repr(row0))
    out = m.run("echo cooked again")
    t.check("the terminal is cooked again after gauge", "cooked again" in out, out)
    m.send("gauge\r")
    m.wait_row(0, " Gauge ")
    m.send("\x03")
    m.expect(PROMPT, 5)
    out = m.run("echo after ctrl-c")
    t.check("Ctrl+C quits gauge too, leaving a usable terminal", "after ctrl-c" in out, out)

    # The desktop: graphics mode, then Esc back to text.
    m.send("run kenoma\r")
    # QEMU shows mode 13h pixel-doubled, so 320x200 dumps as 640x400.
    size = None
    for _ in range(30):
        time.sleep(0.2)
        size = m.screen_size()
        if size == (640, 400):
            break
    t.check("kenoma switches to mode 13h graphics", size == (640, 400), f"screen is {size}")
    # Click the WELCOME page icon (the first slot, at 80,26): the pointer
    # is pinned to the corner first, since mouse moves are relative.
    m.monitor_cmd("mouse_move -400 -400")
    m.monitor_cmd("mouse_move 95 40")
    time.sleep(0.3)
    m.monitor_cmd("mouse_button 1")
    time.sleep(0.2)
    m.monitor_cmd("mouse_button 0")
    row = m.wait_row(0, "Welcome to RingTail.")
    t.check("a text file on the desktop opens in Octavo", row.startswith("Welcome to RingTail."), repr(row))
    m.send("\x11")  # Ctrl+Q, nothing changed so no question
    size = None
    for _ in range(30):
        time.sleep(0.2)
        size = m.screen_size()
        if size == (640, 400):
            break
    t.check("closing Octavo goes straight back to the desktop", size == (640, 400), f"screen is {size}")
    m.monitor_cmd("sendkey esc")
    m.expect(PROMPT, 5)
    t.check("Esc leaves kenoma and text mode returns", m.screen_size() == (720, 400))

    out = m.run("mem")
    t.check("mem", "Frames:" in out and "Heap:" in out, out)

    m.send("reboot\r")
    out = m.expect(PROMPT, 30)
    t.check("reboot comes back up", "RingTail OS" in out, out)
    out = m.run("cat keep.txt")
    t.check("files survive a reboot", "survives a power cycle" in out, out)
    out = m.run("cat keepdir/deep.txt")
    t.check("directories survive a reboot", "nested and kept" in out, out)
    m.send("poweroff\r")
    m.close()
    t.check("poweroff stops the machine", m.qemu.returncode is not None)


def dns_tests(m, t):
    """host and the resolver, against a DNS server of our own on the
    host, which the guest reaches as 10.0.2.2 on whatever port it got."""
    srv = dns_server.Server()
    try:
        _dns_tests(m, t, srv)
    finally:
        srv.close()


def _dns_tests(m, t, srv):
    mem_before = m.run("mem")
    host = lambda name, timeout=15: plain(m.run(f"host -p {srv.port} {name} 10.0.2.2", timeout=timeout)) + "\n"
    out = host("a.test")
    t.check("host looks a name up", "a.test has address 192.0.2.1\n" in out and "exit status" not in out, out)
    q = srv.zone.asked[-1] if srv.zone.asked else None
    t.check("asking for recursion", q and q[1] == dns_server.A and q[3] & 0x0100, str(q))
    out = host("multi.test")
    t.check("every address once, and only class IN", out.count("has address") == 3 and
            all(f"multi.test has address 192.0.2.{i}\n" in out for i in (1, 2, 3)), out)
    for name in ("alias.test", "lone.test"):
        n = len(srv.zone.asked)
        out = host(name)
        t.check(f"an alias is followed ({name})", f"{name} is an alias for a.test.\na.test has address 192.0.2.1\n" in out, out)
    t.check("and asked about again when the answer stops at it", len(srv.zone.asked) - n == 2 and
            srv.zone.asked[-1][0] == "a.test", str(srv.zone.asked[-3:]))
    out = host("shout.test")
    t.check("an answer in other letters is the same name", "shout.test has address 192.0.2.4" in out, out)
    out = host("forged.test")
    t.check("an answer with the wrong ID or question is not believed", "forged.test has address 192.0.2.9\n" in out and
            "6.6.6.6" not in out, out)
    out = host("evil.test")
    t.check("bytes a terminal would act on are printed escaped",
            "evil.test is an alias for e\\027\\0912Jx.test." in out and "\x1b[2J" not in out, repr(out))
    for name, why in [("nope.test", "no such name"), ("noaddr.test", "the name has no address"),
                      ("fail.test", "the DNS server failed to find out"), ("refused.test", "the DNS server refused to answer"),
                      ("loop.test", "too many aliases"), ("big.test", "the answer is too big for UDP (no retry over TCP)"),
                      ("ptrloop.test", "the DNS server's answer made no sense"),
                      ("lies.test", "the DNS server's answer made no sense")]:
        out = host(name)
        t.check(f"{name}: {why}", f"host: {name}: {why}\n" in out and "[exit status 1]" in out, out)
    start = time.time()
    out = host("late.test")
    took = time.time() - start
    t.check("an unanswered question is asked again", "late.test has address 192.0.2.5" in out and 1.5 < took < 3.5,
            "%.2f s: %s" % (took, out))
    start = time.time()
    out = host("slow.test")
    took = time.time() - start
    t.check("and given up on after two tries of 2 s", "host: slow.test: no answer from the DNS server" in out and
            3.5 < took < 6, "%.2f s: %s" % (took, out))
    ids = [q[2] for q in srv.zone.asked]
    t.check("each question gets its own ID; a retry keeps it", srv.zone.asked[-1][2] == srv.zone.asked[-2][2] and
            len(set(ids)) >= len(ids) - 4, str(ids))

    out = host("192.0.2.1")
    t.check("an address is looked up backwards", "1.2.0.192.in-addr.arpa domain name pointer a.test.\n" in out, out)
    out = host("192.0.2.3")
    t.check("through an alias, odd bytes and all",
            "3.2.0.192.in-addr.arpa is an alias for 3.0\\04726.2.0.192.in-addr.arpa.\n"
            "3.0\\04726.2.0.192.in-addr.arpa domain name pointer c.test.\n" in out, out)
    out = host("192.0.2.2")
    t.check("an address with no name", "2.2.0.192.in-addr.arpa has no name\n" in out and "[exit status 1]" in out, out)
    out = host("9.9.9.9")
    t.check("an address nobody knows", "host: 9.9.9.9: no such name" in out, out)

    n = len(srv.zone.asked)
    out = plain(m.run("host localhost")) + "\n"
    t.check("localhost needs no server", "localhost has address 127.0.0.1\n" in out, out)
    out = plain(m.run("host 127.0.0.1")) + "\n"
    t.check("nor does its address", "1.0.0.127.in-addr.arpa domain name pointer localhost.\n" in out, out)
    t.check("none of which asked ours", len(srv.zone.asked) == n, str(srv.zone.asked[n:]))
    for bad in ["", "-p", "-p 0 a.test", "-p 65536 a.test", "-x a.test", "a b c"]:
        out = m.run("host " + bad)
        t.check(f"host {bad or '(nothing)'} is a usage error", "usage: host" in out and "[exit status 2]" in out, out)
    out = m.run("host a.test nameserver")
    t.check("the server must be an address", "the server must be an address" in out and "[exit status 2]" in out, out)
    out = m.run("host bad..name")
    t.check("and the name a name", "host: bad..name: not a valid name" in out and "[exit status 1]" in out, out)

    out = m.run("net")
    t.check("the resolver closes its sockets", "UDP:      0 sockets" in out, out)
    t.check("and leaves memory as it found it", m.run("mem") == mem_before, mem_before)


def ntp_tests(m, t):
    """sntp and SYS_SETTIME, against a time server of our own on the host
    (tests/ntp_server.py). The clock goes back to the host's at the end,
    whatever happened, since the TLS tests check certificates against it."""
    srv = ntp_server.Server()
    try:
        _ntp_tests(m, t, srv)
    finally:
        srv.set("good")
        m.run(f"sntp -p {srv.port} 10.0.2.2", timeout=20)
        srv.close()


def _ntp_tests(m, t, srv):
    # Earlier tests leave connections in TIME-WAIT (30 s), and these tests
    # outlast it: let them go first, or their freeing looks like a change.
    end = time.time() + 40
    while "TCP:      0 connections" not in m.run("net") and time.time() < end:
        time.sleep(1)
    mem_before = m.run("mem")
    epoch = lambda *a: datetime.datetime(*a, tzinfo=datetime.timezone.utc).timestamp()

    def sntp(args, timeout=20):
        start = time.time()
        out = plain(m.run(f"sntp -p {srv.port} {args}", timeout=timeout)) + "\n"
        return out, time.time() - start

    def skew(target=None):  # the guest's clock less target (the host's UTC by default), in seconds
        out = plain(m.run("date"))
        mt = re.match(r"(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)", out.strip())
        guest = datetime.datetime(*map(int, mt.groups())) if mt else None
        return (guest - (target or utcnow())).total_seconds() if guest else 1e9

    def offset(out):  # (stratum, offset, delay) from sntp's report, or None
        mo = re.search(r"10\.0\.2\.2: stratum (\d+), offset ([+-]\d+\.\d{3}) s, delay (\d+\.\d{3}) s\n", out)
        return (int(mo.group(1)), float(mo.group(2)), float(mo.group(3))) if mo else None

    srv.set("good")
    out, _ = sntp("10.0.2.2")
    r = offset(out)
    t.check("sntp asks and says how far off the clock is", r and r[0] == 2 and abs(r[1]) < 1.5 and r[2] < 0.5, out)
    t.check("a clock that is right is left alone", "left as it is" in out or "is now" in out and abs(skew()) < 3, out)
    q = srv.requests[0][0] if srv.requests else b""
    t.check("the request is 48 bytes, version 4, client mode", len(q) == 48 and q[0] == 0x23, q.hex())
    t.check("and says nothing else but a random transmit timestamp", q[1:40] == bytes(39) and
            abs(int.from_bytes(q[40:44], "big") - (int(time.time()) + ntp_server.EPOCH) % 2 ** 32) > 86400, q.hex())

    srv.set("good", offset=3600)
    out, _ = sntp("-q 10.0.2.2")
    r = offset(out)
    t.check("-q measures an hour slow", r and abs(r[1] - 3600) < 1.5, out)
    t.check("and only measures", "clock was" not in out and abs(skew()) < 3, out)
    out, _ = sntp("10.0.2.2")
    t.check("without -q the clock is set", "the clock was " in out and " and is now " in out and
            "exit status" not in out, out)
    t.check("an hour on", abs(skew() - 3600) < 3, out)
    out, _ = sntp("-q 10.0.2.2")
    r = offset(out)
    t.check("to within a tenth of a second", r and abs(r[1]) < 0.1, out)
    srv.set("good")
    out, _ = sntp("10.0.2.2")
    r = offset(out)
    t.check("and back an hour", r and abs(r[1] + 3600) < 1.5 and abs(skew()) < 3, out)

    for mode, good in [("forged", "a reply to some other request is not believed"), ("v3", "a version 3 reply"),
                       ("leap", "a reply announcing a leap second")]:
        srv.set(mode)
        out, _ = sntp("-q 10.0.2.2")
        r = offset(out)
        t.check(good, r and abs(r[1]) < 1.5 and len(srv.requests) == 1, out)
    srv.set("slow")
    out, _ = sntp("-q 10.0.2.2")
    r = offset(out)
    t.check("a server that takes half a second: the round trip leaves it out", r and r[2] < 0.3 and abs(r[1]) < 1.5, out)

    for mode, why in [("kiss", "the server sent a kiss-o'-death (RATE)"),
                      ("kiss-evil", "the server sent a kiss-o'-death (\\027[2J)"),
                      ("unsync", "the server does not know the time itself"),
                      ("stratum16", "the server does not know the time itself"),
                      ("zero", "the answer made no sense")]:
        srv.set(mode)
        out, took = sntp("10.0.2.2")
        t.check(f"{mode}: {why}", f"sntp: 10.0.2.2: {why}\n" in out and "[exit status 1]" in out and
                "\x1b[2J" not in out, repr(out))
        t.check(f"{mode}: and no second try", len(srv.requests) == 1 and took < 2.5, "%d requests" % len(srv.requests))
    t.check("none of them touched the clock", abs(skew()) < 3)

    for mode, what in [("silent", "silence"), ("mode3", "a reply in client mode"), ("short", "a short reply")]:
        srv.set(mode)
        out, took = sntp("10.0.2.2")
        t.check(f"{what} is no answer, after two tries of 2 s", "sntp: 10.0.2.2: no answer\n" in out and
                "[exit status 1]" in out and 3.5 < took < 6.5 and len(srv.requests) == 2, "%.2f s: %s" % (took, out))
    nonces = [q[40:48] for q, _ in srv.requests]
    t.check("each try with its own nonce", len(set(nonces)) == 2, str(nonces))
    srv.set("late")
    out, took = sntp("-q 10.0.2.2")
    t.check("an unanswered request is sent again", offset(out) and len(srv.requests) == 2 and 1.5 < took < 4,
            "%.2f s: %s" % (took, out))

    srv.set("good", at=epoch(2001, 1, 1))
    out, _ = sntp("10.0.2.2")
    t.check("a time before the system was built is refused by the kernel",
            "sntp: the server says it is 2001-01-01 00:00:0" in out and
            "before this system was built; the clock is left alone\n" in out and "[exit status 1]" in out, out)
    srv.set("good", at=epoch(2101, 1, 1))
    out, _ = sntp("10.0.2.2")
    t.check("a time 75 years on cannot be placed", "more than 68 years from ours" in out and "[exit status 1]" in out, out)
    t.check("and the clock is still right", abs(skew()) < 3)

    out = plain(m.run("rtcset"))
    t.check("SYS_SETTIME refuses dates that do not exist, and times it will not hold",
            "feb29in2100=-22 apr31=-22 month0=-22 month13=-22 day0=-22 hour24=-22 min60=-22 sec60=-22 year1969=-22 "
            "year2100=-34 year2106=-34 year2001=-34 year2020=-34 null=-14 kernel=-14" in out, out)
    out = plain(m.run("rtcset 2031 5 6 7 8 9"))
    t.check("and sets one that does", re.search(r"set=0 read=2031-05-06 07:08:(09|10)", out), out)
    m.run("write ntpstamp.txt x")
    out = plain(m.run("ls ntpstamp.txt"))
    t.check("a file written then is stamped with it", "2031-05-06 07:08" in out, out)
    m.run("rm ntpstamp.txt")
    out = plain(m.run("rtcset 2028 2 29 23 59 58"))
    t.check("a leap day", "set=0 read=2028-02-29 23:59:5" in out, out)
    time.sleep(2.5)
    t.check("which the clock runs on into March", abs(skew(datetime.datetime(2028, 3, 1, 0, 0, 0))) < 3)
    srv.set("good")
    out, _ = sntp("10.0.2.2")
    t.check("sntp puts it right again", "the clock was 2028-03-01 00:00:" in out and abs(skew()) < 3, out)

    # -b, as init runs it at boot: one line, one quick try.
    srv.set("good", offset=7200)  # forward: back could cross the build's floor
    out, _ = sntp("-b 10.0.2.2")
    t.check("sntp -b sets the clock and says so in one line",
            re.fullmatch(r"Clock: set to \d{4}-\d\d-\d\d \d\d:\d\d:\d\d by 10\.0\.2\.2 \(it was 7\d{3}\.\d{3} s slow\)\n", out)
            and abs(skew() - 7200) < 3, out)
    srv.set("good")
    out, _ = sntp("-b -q 10.0.2.2")
    t.check("-b -q says how far off and leaves it", re.fullmatch(
            r"Clock: \d{4}-\d\d-\d\d \d\d:\d\d:\d\d, 7\d{3}\.\d{3} s fast by 10\.0\.2\.2; left as it is\n", out)
            and abs(skew() - 7200) < 3, out)
    out, _ = sntp("-b 10.0.2.2")
    t.check("and without -q puts it right", "Clock: set to " in out and "s fast)" in out and abs(skew()) < 3, out)
    out, _ = sntp("-b 10.0.2.2")
    t.check("a clock that is right is said to be", re.fullmatch(
            r"Clock: \d{4}-\d\d-\d\d \d\d:\d\d:\d\d, right to within half a second by 10\.0\.2\.2\n", out), out)
    srv.set("silent")
    out, took = sntp("-b 10.0.2.2")
    t.check("with no answer, -b gives up after one try of a second, in one line", re.fullmatch(
            r"Clock: \d{4}-\d\d-\d\d \d\d:\d\d:\d\d, as the CMOS has it \(10\.0\.2\.2: no answer\)\n\[exit status 1\]\n", out)
            and len(srv.requests) == 1 and 0.5 < took < 2.5, "%.2f s, %d requests: %r" % (took, len(srv.requests), out))
    srv.set("good", at=epoch(2001, 1, 1))
    out, _ = sntp("-b 10.0.2.2")
    t.check("and a time before the build is turned down in the same line",
            "as the CMOS has it (10.0.2.2: says it is 2001-01-01 00:00:0" in out and
            ", before this system was built)\n[exit status 1]\n" in out and out.count("\n") == 2 and abs(skew()) < 3, out)
    out = plain(m.run("net"))
    t.check("QEMU's DHCP names no time server", "Time:" not in out, out)

    for bad in ["-x", "-p", "-p 0 a", "-p 65536 a", "a b"]:
        out = m.run("sntp " + bad)
        t.check(f"sntp {bad} is a usage error", "usage: sntp" in out and "[exit status 2]" in out, out)
    out = m.run("net")
    t.check("sntp closes its socket", "UDP:      0 sockets" in out, out)
    t.check("and leaves memory as it found it", m.run("mem") == mem_before, mem_before)


BIG = bytes((i * 7919 + (i >> 8)) & 255 for i in range(300000))

# Data for sort and uniq, served as pages and fetched into the guest.
WORDS = ["pear", "apple", "Banana", "apple", "cherry", "Apple"]
NUMS = ["10", "9", "-3", "2.50", "2.5", "  7", "abc", "-0", "007"]
TBL = ["b 2 x", "a 10 y", "c 1 z", "b 3 w", "a 9 v"]
COLON = ["a:3", "b:1", "c:2", "d:10"]
BLANKS = ["x  b", "x a", "x   c"]
DUPS = ["x", "x", "y", "X", "z", "z", "z", "", "", "q"]  # the last has no newline
ROWS = ["%d %d w%d" % ((i * 7919) % 1500, i, i % 97) for i in range(6000)]
LONG = ["a" * 3000, "a" * 3000, "a" * 2999 + "b"]
# For hexdump: a run of zeros, a line repeated, then an odd tail, so the
# listing has "*" in it and ends on a half word.
REP = bytes(100) + b"0123456789abcdef" * 5 + b"tail!"
DATA = {"words": WORDS, "nums": NUMS, "tbl": TBL, "colon": COLON, "blanks": BLANKS, "dups": DUPS,
        "rows": ROWS, "long": LONG}


def _chunked(data, sizes, last=b"0\r\n\r\n"):
    """data as a chunked body, in pieces of these sizes over and over,
    written the three ways a length may be: lower case, upper case, and
    with an extension after it."""
    out, at, i = [], 0, 0
    while at < len(data):
        n = min(sizes[i % len(sizes)], len(data) - at)
        length = [b"%x", b"%X", b"%x;note=\"a;b\""][i % 3] % n
        out.append(length + b"\r\n" + data[at:at + n] + b"\r\n")
        at += n
        i += 1
    return b"".join(out) + last


class _Http(http.server.BaseHTTPRequestHandler):
    """A few pages for fetch: plain, redirected every way, missing, cut
    short, big, and the shapes a response has in HTTP/1.1 (in chunks,
    after an interim response, with no body, with no stated length).
    The same pages are served over TLS by a second server; each knows
    the other's port (server.other) so a redirect can cross over."""
    timeout = 30
    extra = {}  # more pages, which a test puts here

    def do_GET(self):
        pages = {"/hello.txt": b"hello from the host\n", "/rel/b": b"relative ok\n", "/big.bin": BIG,
                 "/rep.bin": REP, "/zero.bin": bytes(64), **self.extra}
        for name, rows in DATA.items():
            pages["/%s.txt" % name] = "\n".join(rows).encode() + (b"" if name == "dups" else b"\n")
        here, other = self.server.server_port, getattr(self.server, "other", 0)
        secure = isinstance(self.connection, ssl.SSLSocket)
        moves = {"/moved": "/hello.txt", "/rel/a": "b", "/loop": "/loop",
                 "/abs": "http://10.0.2.2:%d/hello.txt" % (other if secure else here),
                 "/secure": "https://10.0.2.2:%d/hello.txt" % (here if secure else other),
                 "/same": "//10.0.2.2:%d/hello.txt" % here}
        head = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        # Sent as they are, each piece in a segment of its own.
        raw = {"/chunked": [head] + [_chunked(BIG, [1, 2, 4095, 4096, 4097, 17, 65536],
                                              b"0\r\nChecked: no\r\nMore: yes\r\n\r\nNOT THE BODY")],
               "/chunked-slowly": [head] + [bytes([b]) for b in _chunked(b"one piece at a time\n", [7, 1, 11])],
               "/chunked-cut": [head, _chunked(b"all of this, and then nothing\n", [10], b"")],
               "/chunked-bad": [head, b"5\r\nhello\r\nzz\r\nworld\r\n0\r\n\r\n"],
               "/chunked-huge": [head, b"100000000\r\n"],
               "/gzip": [b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n"],
               "/hints": [b"HTTP/1.1 103 Early Hints\r\nLink: </x.css>\r\n\r\n", b"HTTP/1.1 100 Continue\r\n\r\n"
                          b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nafter a hint\n"],
               "/nothing": [b"HTTP/1.1 204 No Content\r\n\r\n"],
               "/unsized": [b"HTTP/1.1 200 OK\r\n\r\n", b"as long as ", b"it lasts\n"],
               "/cut": [b"HTTP/1.1 200 OK\r\n\r\n", b"as long as ", b"it lasts\n"]}
        if self.path in raw:
            for piece in raw[self.path]:
                self.wfile.write(piece)
                self.wfile.flush()
                if len(piece) < 100:
                    time.sleep(0.005)
            # Over TLS a server says when it is done; "/cut" and the
            # broken ones do not, like a connection someone has cut.
            if secure and self.path in ("/unsized", "/nothing", "/chunked"):
                self.connection.unwrap()
            self.close_connection = True
        elif self.path in moves:
            self.send_response(302)
            self.send_header("Location", moves[self.path])
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/short":
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"only a bit\n")
        elif self.path in pages:
            self.server.agents.append(self.headers.get("User-Agent", ""))
            self.send_response(200)
            self.send_header("Content-Length", str(len(pages[self.path])))
            self.end_headers()
            self.wfile.write(pages[self.path])
        else:
            self.send_error(404, "Not Found")

    def log_message(self, *a):
        pass


def _echo_once(srv, got):
    c, _ = srv.accept()
    with c:
        while True:
            d = c.recv(4096)
            if not d:
                break
            got.append(d)
            c.sendall(d.upper())


def sortuniq_tests(m, t, url):
    """sort and uniq, on small files with every flag and then on 6000
    lines and a 300000-byte binary file. The guest has no cmp, so a big
    result is checked as sorted (sort -c), counted, and read at a few
    places."""
    import itertools
    from fractions import Fraction

    def run(c, timeout=30):  # output as it is, indentation included
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    def want(rows):
        return "\n".join(rows).rstrip()

    def num(l):  # what sort -n reads off the front of a line
        r = re.match(r"[ \t]*(-?\d+(\.\d*)?|-?\.\d+)", l)
        return Fraction(r.group(1)) if r else Fraction(0)

    def bytewise(l):
        return l.encode()

    def first_of_each(rows, key):  # sort -u: the first line of each run of equal keys, in key order
        seen = {}
        for r in rows:
            seen.setdefault(key(r), r)
        return [seen[k] for k in sorted(seen)]

    for f in DATA:
        out = run("fetch -o %s.txt %s/%s.txt" % (f, url, f), timeout=60)
        t.check("fetch %s.txt (setting up)" % f, "saved" in out, out)

    # text order is bytes: capitals first
    t.check("sort", run("sort words.txt") == want(sorted(WORDS, key=bytewise)))
    t.check("sort -r reverses it", run("sort -r words.txt") == want(sorted(WORDS, key=bytewise, reverse=True)))
    t.check("sort -u drops repeats", run("sort -u words.txt") == want(sorted(set(WORDS), key=bytewise)))
    ci = lambda l: l.upper().encode()
    t.check("sort -f ignores case, then falls back to the bytes",
            run("sort -f words.txt") == want(sorted(WORDS, key=lambda l: (ci(l), bytewise(l)))))
    t.check("sort -fu keeps the first of each, in input order",
            run("sort -fu words.txt") == want(first_of_each(WORDS, ci)))
    t.check("-s keeps input order for equal keys", run("sort -fs words.txt") == want(sorted(WORDS, key=ci)))
    t.check("sort reads a pipe", run("cat words.txt | sort") == want(sorted(WORDS, key=bytewise)))
    t.check("and - means standard input", run("cat words.txt | sort -r -") == want(sorted(WORDS, key=bytewise, reverse=True)))
    t.check("sort puts two files together", run("sort words.txt words.txt | wc").split()[0] == "12")
    t.check("options may follow the files", run("sort words.txt -r") == run("sort -r words.txt"))

    nk = lambda l: (num(l), bytewise(l))
    t.check("sort -n: blanks, signs, fractions, junk as zero, -0 as 0",
            run("sort -n nums.txt") == want(sorted(NUMS, key=nk)))
    t.check("sort -nr", run("sort -nr nums.txt") == want(sorted(NUMS, key=nk, reverse=True)))
    t.check("sort -nu keeps the first of each value", run("sort -nu nums.txt") == want(first_of_each(NUMS, num)))

    f2 = lambda l: l.split()[1]
    t.check("-k 2 sorts on field 2 and what follows, blank included",
            run("sort -k 2 tbl.txt") == want(sorted(TBL, key=lambda l: (" " + f2(l) + " " + l.split()[2]).encode())))
    t.check("-k 2n", run("sort -k 2n tbl.txt") == want(sorted(TBL, key=lambda l: (int(f2(l)), bytewise(l)))))
    t.check("-k2,2n, with the flag after the key", run("sort -k2,2n tbl.txt") == run("sort -n -k 2,2 tbl.txt") ==
            want(sorted(TBL, key=lambda l: (int(f2(l)), bytewise(l)))))
    t.check("-k 3,3 -r", run("sort -k 3,3 -r tbl.txt") == want(sorted(TBL, key=lambda l: l.split()[2], reverse=True)))
    t.check("-u -k1,1 keeps the first line of each key",
            run("sort -u -k1,1 tbl.txt") == want(first_of_each(TBL, lambda l: l.split()[0])))
    t.check("-t : -k 2n", run("sort -t : -k 2n colon.txt") == want(sorted(COLON, key=lambda l: int(l.split(":")[1]))))
    t.check("blanks lead a field", run("sort -k 2 blanks.txt") == want(["x   c", "x  b", "x a"]))
    t.check("and -b skips them", run("sort -b -k 2 blanks.txt") == want(["x a", "x  b", "x   c"]))

    # -c, -o, and what goes wrong
    out = run("sort -c words.txt")
    t.check("sort -c names the first line out of order", out == "sort: words.txt:2: disorder: apple\n[exit status 1]", out)
    t.check("sort words.txt -o sorted.txt (setting up)", run("sort words.txt -o sorted.txt") == "")
    t.check("sort -c is quiet when it is in order", run("sort -c sorted.txt") == "")
    out = run("sort -cu sorted.txt")
    t.check("and -cu counts a repeat as disorder", "sorted.txt:4: disorder: apple" in out and "exit status 1" in out, out)
    t.check("-o may name an input", run("sort -r -o sorted.txt sorted.txt") == "" and
            run("cat sorted.txt | wc").split()[0] == "6" and run("sort -c -r sorted.txt") == "")
    out = run("sort nosuch")
    t.check("sort says when a file is missing", "sort: nosuch: no such file" in out and "exit status 2" in out, out)
    out = run("sort -k 0 words.txt")
    t.check("sort rejects a bad key", "sort: invalid key: '0'" in out and "exit status 2" in out, out)
    out = run("sort -k 1 -k 2 words.txt")
    t.check("and a second key", "only one -k key" in out and "exit status 2" in out, out)
    out = run("sort -z words.txt")
    t.check("sort rejects an unknown option", "usage: sort" in out and "exit status 2" in out, out)

    # uniq
    run_len = lambda rows: [(len(list(g)), k) for k, g in itertools.groupby(rows)]
    counted = want(["%7d %s" % c for c in run_len(DUPS)])
    t.check("uniq -c", run("uniq -c dups.txt") == counted, run("uniq -c dups.txt"))
    t.check("uniq collapses runs, a last line without a newline too",
            run("uniq dups.txt") == want([k for _, k in run_len(DUPS)]))
    t.check("uniq -d", run("uniq -d dups.txt") == "x\nz")
    t.check("uniq -u", run("uniq -u dups.txt") == "y\nX\nq")
    t.check("uniq -i", run("uniq -i dups.txt") == want(["x", "y", "X", "z", "", "q"]))
    t.check("sort | uniq -c", run("sort dups.txt | uniq -c") ==
            want(["%7d %s" % c for c in run_len(sorted(DUPS, key=bytewise))]))
    t.check("uniq -f 1 skips a field", run("uniq -f 1 tbl.txt") == want(TBL))
    t.check("uniq -s 1 -w 1 compares one character", run("uniq -s 1 -w 1 words.txt") == want(WORDS))
    t.check("-w 1 makes neighbours alike", run("uniq -w 1 dups.txt") == want(["x", "y", "X", "z", "", "q"]))
    t.check("uniq reads a pipe", run("cat dups.txt | uniq -d") == "x\nz")
    t.check("uniq INPUT OUTPUT", run("uniq dups.txt out.txt") == "" and run("cat out.txt") == want([k for _, k in run_len(DUPS)]))
    t.check("uniq grows its buffers for long lines",
            run("uniq -c long.txt | wc").split()[:2] == ["2", "4"] and run("uniq -d long.txt | wc").split()[0] == "1")
    out = run("uniq nosuch")
    t.check("uniq says when a file is missing", "uniq: nosuch: no such file" in out and "exit status 1" in out, out)
    out = run("uniq -f x dups.txt")
    t.check("uniq rejects a bad count", "invalid number of fields to skip: 'x'" in out and "exit status 2" in out, out)
    out = run("uniq -z")
    t.check("uniq rejects an unknown option", "usage: uniq" in out and "exit status 2" in out, out)
    out = run("uniq a b c")
    t.check("and a third operand", "extra operand 'c'" in out and "exit status 2" in out, out)

    # bigger
    def at(f, k):
        return run("tail -n +%d %s | head -n 1" % (k, f))

    def spot(label, f, ref):
        n = len(ref)
        t.check("%s: %d lines" % (label, n), run("wc " + f).split()[0] == str(n), run("wc " + f))
        bad = [k for k in (1, 2, 3, 777, 1500, n // 2, n - 1, n) if at(f, k) != ref[k - 1]]
        t.check("%s: the lines at 1, 2, 3, 777, ... are right" % label, not bad, str(bad))

    by_num = sorted(ROWS, key=lambda l: (num(l), bytewise(l)))
    by_text = sorted(ROWS, key=bytewise)
    t.check("sort -n on 6000 lines (setting up)", run("sort -n rows.txt > byn.txt", timeout=120) == "")
    t.check("sort -nc says it is in order", run("sort -nc byn.txt") == "")
    spot("sort -n", "byn.txt", by_num)
    t.check("sort on 6000 lines (setting up)", run("sort rows.txt > byt.txt", timeout=120) == "")
    t.check("sort -c says it is in order", run("sort -c byt.txt") == "")
    spot("sort", "byt.txt", by_text)
    t.check("sort -r on the sorted file reverses it (setting up)", run("sort -r byt.txt > rev.txt", timeout=120) == "")
    spot("sort -r", "rev.txt", by_text[::-1])
    uniq_keys = first_of_each(ROWS, lambda l: int(l.split()[0]))
    t.check("sort -u -k1,1n gives the first row of each key (setting up)",
            run("sort -u -k1,1n rows.txt > byk.txt", timeout=120) == "")
    spot("sort -u -k1,1n", "byk.txt", uniq_keys)
    t.check("every row differs, so uniq -c has 6000 lines", run("sort -n rows.txt | uniq -c | wc", timeout=120).split()[0] == "6000")

    data = BIG if BIG.endswith(b"\n") else BIG + b"\n"
    for c in ["sort big.bin | wc", "sort -r big.bin | wc", "sort -n big.bin | wc"]:
        r = re.search(r"(\d+)\s+(\d+)\s+(\d+)", run(c, timeout=120))
        t.check(c + " keeps every byte of a binary file", r and (int(r.group(1)), int(r.group(3))) == (data.count(b"\n"), len(data)),
                r.group(0) if r else "")

    for f in list(DATA) + ["byn", "byt", "rev", "byk", "sorted", "out"]:
        run("rm %s.txt" % f)


def coreutils_tests(m, t):
    """The file commands that used to be built into the shell and are
    programs now: ls, cat, echo, mkdir, rmdir, touch, rm and mv, as
    programs do things a built-in could not (pipes, stdin, exit
    status), and with several arguments."""
    def run(c, timeout=30):
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    t.check("the eight are programs on the disk",
            all(re.search(r"^\s+%s\s" % n, run("ls /"), re.M) for n in ["LS", "CAT", "RM", "MV", "MKDIR", "RMDIR", "TOUCH", "ECHO"]))
    t.check("echo", run("echo hello world") == "hello world")
    t.check("echo -n leaves off the newline", run("echo -n hi | wc").split()[2] == "2")
    t.check("echo with nothing is a newline", run("echo | wc").split()[2] == "1")
    t.check("quotes keep blanks together", run("echo 'a   b' c") == "a   b c")
    t.check("echo reaches the end of a pipe", run("echo hi | cat | cat") == "hi")
    t.check("cat reads standard input", run("echo piped | cat") == "piped")
    t.check("cat - too", run("echo piped | cat -") == "piped")
    t.check("cat counts its input", run("echo hello | cat | wc").split()[2] == "6")
    out = run("cat -x")
    t.check("an unknown option shows the usage and exits 2", "usage: cat" in out and "[exit status 2]" in out, out)

    run("mkdir ca cb cc")
    t.check("mkdir takes several", all(d in run("ls") for d in ["ca/", "cb/", "cc/"]))
    out = run("mkdir cd ca ce")
    t.check("mkdir reports one that exists and still makes the rest",
            "mkdir: ca: already exists" in out and "[exit status 1]" in out and "cd/" in run("ls") and "ce/" in run("ls"), out)
    t.check("mkdir with nothing shows the usage", "usage: mkdir" in run("mkdir"))
    t.check("an unknown mkdir option shows the usage", "usage: mkdir" in run("mkdir -Z x") and "-Z" not in run("ls"))
    out = run("mkdir bad*name")
    t.check("a bad name says so", "not a valid file name" in out, out)

    run("touch ta tb tc")
    t.check("touch takes several", all(f in run("ls") for f in ["ta ", "tb ", "tc "]))
    out = run("touch td ta te")
    t.check("touch leaves one that exists alone and makes the rest",
            out == "" and re.search(r"\bte\b", run("ls")) and re.search(r"\btd\b", run("ls")), out)
    t.check("touch with nothing shows the usage", "usage: touch" in run("touch"))

    run("write cat1.txt one")
    run("write cat2.txt two")
    out = run("ls | wc")
    n = int(out.split()[0])
    t.check("ls into a pipe is one line per entry", n >= 10, out)
    run("write ca/x.txt xx")
    out = run("ls ca")
    t.check("ls DIR lists what is in it", "x.txt" in out and "KB free" in out, out)
    out = run("ls nope")
    t.check("ls of a missing directory", "ls: nope: no such file" in out and "[exit status 1]" in out, out)
    t.check("an unknown ls option shows the usage", "usage: ls" in run("ls -Z"))
    out = run("ls | sort | head -n 3")
    t.check("ls feeds a pipeline", len(out.split("\n")) == 3, out)
    t.check("ls takes a path with a slash", "x.txt" in run("ls /ca") and "x.txt" in run("ls ./ca"))

    out = run("rm cat1.txt nope.txt cat2.txt")
    t.check("rm reports a missing one and still removes the rest",
            "rm: nope.txt: no such file" in out and "[exit status 1]" in out and "cat1" not in run("ls") and "cat2" not in run("ls"), out)
    out = run("rm ca")
    t.check("rm of a directory is refused with a hint", "is a directory (rm -r removes it" in out, out)
    t.check("rm with nothing shows the usage", "usage: rm" in run("rm"))
    t.check("rm -- takes a name that starts with a dash", run("touch ./-dash") == "" and run("rm -- -dash") == "" and "-dash" not in run("ls"))

    out = run("rmdir ca")
    t.check("rmdir of a directory with something in it is refused", "rmdir: ca:" in out and "[exit status 1]" in out, out)
    run("cd cb")
    out = run("rmdir /cb")
    t.check("rmdir of the current directory says it is in use", "in use" in out, out)
    run("cd ..")
    out = run("rmdir cb cc cd nope ce")
    t.check("rmdir takes several and reports the one that fails",
            "rmdir: nope: no such file" in out and "[exit status 1]" in out and not any(d in run("ls") for d in ["cb/", "cc/", "cd/", "ce/"]), out)
    t.check("rmdir with nothing shows the usage", "usage: rmdir" in run("rmdir"))

    run("mkdir mvdir")
    run("mv ta tb tc mvdir")
    out = run("ls mvdir")
    t.check("mv takes several into a directory", all(f in out for f in ["ta ", "tb ", "tc "]) and "ta " not in run("ls"), out)
    t.check("mv renames", run("mv td tz") == "" and re.search(r"\btz\b", run("ls")) and not re.search(r"\btd\b", run("ls")))
    out = run("mv tz te mvdir/ta")
    t.check("several things need a directory last", "needs a directory last" in out and "[exit status 2]" in out, out)
    out = run("mv tz")
    t.check("mv with one argument shows the usage", "usage: mv" in out and "[exit status 2]" in out, out)
    out = run("mv nope mvdir")
    t.check("mv of a missing file", "mv: nope: no such file" in out and "[exit status 1]" in out, out)
    run("write tz.txt new")
    run("write mvdir/tz.txt old")
    t.check("mv -n leaves an existing file alone and says nothing",
            run("mv -n tz.txt mvdir") == "" and run("cat mvdir/tz.txt") == "old" and run("cat tz.txt") == "new")
    t.check("mv replaces an existing file", run("mv tz.txt mvdir") == "" and run("cat mvdir/tz.txt") == "new"
            and "tz.txt" not in run("ls"))
    run("write tz.txt newer")
    t.check("mv -f says so out loud and does the same",
            run("mv -f tz.txt mvdir/tz.txt") == "" and run("cat mvdir/tz.txt") == "newer")
    run("mkdir mvsub")
    out = run("mv mvsub mvdir/tz.txt")
    t.check("a directory does not replace a file", "never replaced" in out and "[exit status 1]" in out, out)
    run("write tzz.txt file")
    run("mkdir mvdir/tzz.txt")
    out = run("mv tzz.txt mvdir")
    t.check("a file does not replace a directory", "never replaced" in out and "[exit status 1]" in out and "file" in run("cat tzz.txt"), out)
    run("rm tzz.txt")
    run("rmdir mvdir/tzz.txt")
    run("write same.txt s")
    out = run("mv same.txt ./same.txt")
    t.check("mv onto itself says so", "same file" in out and "[exit status 1]" in out and run("cat same.txt") == "s", out)
    run("rm same.txt")
    t.check("mv -f with -n is refused", "usage: mv" in run("mv -f -n tz mvdir"))
    t.check("mv -Z shows the usage", "usage: mv" in run("mv -Z tz mvdir"))
    run("rm mvdir/tz.txt")
    run("rmdir mvsub")

    run("find mvdir -name 't?' -exec rm {} ;")
    out = run("ls mvdir")
    t.check("find -exec rm works now", not any(f in out for f in ["ta ", "tb ", "tc "]), out)
    for c in ["rm tz", "rm te", "rmdir mvdir", "rm ca/x.txt", "rmdir ca"]:
        run(c)


def flags_tests(m, t):
    """The options the eight programs (and cp) learned in v0.24.7: touch -c
    and an existing file, mkdir -p, rm -f and -r, cp -r and -n, mv's
    replace and -n, ls with several paths and -R, cat -n, echo -e."""
    def run(c, timeout=30):
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    def flat(c):  # blanks and tabs collapsed, for output that has tabs in it
        return re.sub(r"\s+", " ", run(c)).strip()

    # ---- echo ----
    lines = run("echo -e 'a\\tb\\nc'").split("\n")
    t.check("echo -e reads escapes", len(lines) == 2 and re.sub(r"\s+", " ", lines[0]) == "a b" and lines[1] == "c", lines)
    t.check("without -e they stay as typed", run("echo 'a\\nb'") == "a\\nb")
    t.check("-E is the same as the default", run("echo -E 'a\\nb'") == "a\\nb")
    t.check("\\c stops the output, newline and all", run("echo -e 'x\\cy' | wc").split()[2] == "1")
    t.check("-n and -e may be bundled", run("echo -ne 'hi' | wc").split()[2] == "2")
    t.check("octal and hex bytes", run("echo -e '\\0101\\x42\\x4a'") == "ABJ")
    t.check("a backslash before nothing known stays", run("echo -e 'a\\qb'") == "a\\qb")
    t.check("a word that is not all n, e and E is text", run("echo -x hi") == "-x hi" and run("echo -- hi") == "-- hi")
    t.check("echo -e writes a reader that left quietly", run("echo -e 'a\\nb' | head -n 1") == "a")

    # ---- cat -n ----
    run("echo -e 'a\\nb' > fl1.txt")
    run("write fl2.txt abc")
    t.check("cat -n numbers lines", flat("cat -n fl1.txt") == "1 a 2 b")
    t.check("and keeps counting into the next file", flat("cat -n fl1.txt fl2.txt") == "1 a 2 b 3 abc")
    t.check("cat -n on standard input", flat("echo hi | cat -n") == "1 hi")
    t.check("cat -n into a pipe is exactly the numbered bytes", run("cat -n fl2.txt | wc").split()[2] == "11")
    t.check("cat -n of an empty line still numbers it", flat("echo -e 'x\\n\\ny' | cat -n") == "1 x 2 3 y")
    t.check("a bad cat option shows the usage", "usage: cat [-n]" in run("cat -Z fl1.txt"))
    run("rm fl1.txt fl2.txt")

    # ---- touch ----
    t.check("touch -c makes nothing", run("touch -c flnew.txt") == "" and "flnew" not in run("ls"))
    t.check("touch makes it", run("touch flnew.txt") == "" and "flnew.txt" in run("ls"))
    t.check("and again is fine", run("touch flnew.txt flnew.txt") == "")
    run("write flnew.txt keep")
    t.check("touch leaves the contents alone", run("touch flnew.txt") == "" and run("cat flnew.txt") == "keep")
    t.check("touch of a directory is fine too", run("mkdir fld") == "" and run("touch fld") == "")
    t.check("a bad touch option shows the usage", "usage: touch [-c]" in run("touch -Z a"))
    out = run("touch flnew.txt/x")
    t.check("touch under a file is an error", "[exit status 1]" in out, out)
    run("rm flnew.txt")
    run("rmdir fld")

    # ---- mkdir -p ----
    t.check("mkdir -p makes the parents", run("mkdir -p flp/a/b/c") == "" and "c/" in run("ls flp/a/b"))
    t.check("and an existing one is no error", run("mkdir -p flp/a/b/c flp/a") == "")
    out = run("mkdir flp/a")
    t.check("without -p an existing one still is", "already exists" in out and "[exit status 1]" in out, out)
    t.check("mkdir -p takes a trailing slash and several", run("mkdir -p flp/t/ flp/u/v") == "" and "v/" in run("ls flp/u"))
    t.check("mkdir -p with an absolute path", run("mkdir -p /flp/abs/x") == "" and "x/" in run("ls /flp/abs"))
    run("touch flp/file")
    out = run("mkdir -p flp/file/x flp/after")
    t.check("mkdir -p through a file fails and the next still goes ahead",
            "flp/file/x: not a directory" in out and "[exit status 1]" in out and "after/" in run("ls flp"), out)
    t.check("mkdir -p of a bad name says so", "not a valid file name" in run("mkdir -p flp/bad*name"))

    # ---- rm -f and -r ----
    t.check("rm -f of nothing is quiet", run("rm -f flp/nothing") == "" and run("rm -f") == "")
    t.check("rm -f still removes", run("rm -f flp/nothing flp/file") == "" and not re.search(r"\bfile\b", run("ls flp")))
    out = run("rm flp/nothing")
    t.check("without -f a missing file is an error", "no such file" in out and "[exit status 1]" in out, out)
    run("write flp/a/f.txt hi")
    run("write flp/a/b/g.txt hi")
    out = run("rm flp/a")
    t.check("rm of a directory without -r says how", "rm -r removes it" in out and "a/" in run("ls flp"), out)
    t.check("rm -r removes a whole tree", run("rm -r flp") == "" and "flp" not in run("ls"))
    run("touch fr1")
    t.check("rm -r on a plain file just removes it", run("rm -r fr1") == "" and "fr1" not in run("ls"))
    run("mkdir -p frx/y")
    t.check("rm -R and -fr", run("rm -fR frx") == "" and "frx" not in run("ls") and run("rm -fr frx nothing") == "")
    out = run("rm -r .")
    t.check("rm -r . is refused", "will not remove" in out and "[exit status 1]" in out, out)
    run("mkdir flz")
    t.check("so are .. and / and trailing-slash spellings",
            all("will not remove" in run("rm -rf " + p) for p in ["..", "/", "./", ".//", "flz/.."]) and "WELCOME.TXT" in run("ls"))
    run("rmdir flz")
    run("mkdir flr")
    run("write flr/x hi")
    run("cd flr")
    out = run("rm -r /flr")
    run("cd ..")
    t.check("rm -r of the directory you are in empties it and says why it stays",
            "in use" in out and "[exit status 1]" in out and "x" not in run("ls flr"), out)
    run("rmdir flr")
    t.check("an unknown rm option shows the usage", "usage: rm [-f] [-r]" in run("rm -Z a"))

    # ---- cp -r and -n ----
    run("mkdir -p fs/sub")
    run("write fs/a.txt AAA")
    run("write fs/sub/b.txt BBB")
    out = run("cp fs fd")
    t.check("cp of a directory without -r says how", "cp -r copies it" in out and "[exit status 1]" in out and not re.search(r"\bfd/", run("ls")), out)
    t.check("cp -r copies a tree", run("cp -r fs fd") == "" and run("cat fd/a.txt") == "AAA" and run("cat fd/sub/b.txt") == "BBB")
    t.check("onto an existing directory it goes inside", run("cp -r fs fd") == "" and run("cat fd/fs/sub/b.txt") == "BBB")
    run("write fs/sub/c.txt CCC")
    t.check("a directory copied onto a directory of that name merges",
            run("cp -R fs/sub fd") == "" and run("cat fd/sub/c.txt") == "CCC" and run("cat fd/sub/b.txt") == "BBB")
    out = run("cp -r fs fs/sub")
    t.check("a directory cannot go into itself", "into itself" in out and "[exit status 1]" in out and "fs/" not in run("ls fs/sub"), out)
    t.check("nor a long way down inside itself", "into itself" in run("cp -r fs fs/sub/deep") and "deep" not in run("ls fs/sub"))
    t.check("nor from inside it", "into itself" in run("cp -r /fs/sub /fs/sub/x") and not re.search(r"\bx/", run("ls fs/sub")))
    run("cd fs")
    out = run("cp -r . sub/z")
    run("cd ..")
    t.check("nor . into a part of itself", "into itself" in out and "z" not in run("ls fs/sub"), out)
    t.check("cp -r of a file is a plain copy", run("cp -r fs/a.txt fd/plain.txt") == "" and run("cat fd/plain.txt") == "AAA")
    run("write fn1 one")
    run("write fn2 two")
    t.check("cp -n leaves an existing target alone", run("cp -n fn1 fn2") == "" and run("cat fn2") == "two")
    t.check("cp -f replaces it", run("cp -f fn1 fn2") == "" and run("cat fn2") == "one")
    t.check("cp -n with -f is refused", "usage: cp" in run("cp -n -f fn1 fn2"))
    t.check("an unknown cp option shows the usage", "usage: cp" in run("cp -Z fn1 fn2"))
    run("rm fn1 fn2")
    run("mkdir fb")
    for k in range(0, 50, 10):
        run("touch " + " ".join("fb/f%02d" % i for i in range(k, k + 10)))
    t.check("a directory of 50 entries (past the first 32) copies whole",
            run("cp -r fb fc") == "" and run("ls fc | wc").split()[0] == "50" and run("ls fb | wc").split()[0] == "50")
    t.check("and rm -r takes it away", run("rm -r fb fc") == "" and not re.search(r"\bfb/", run("ls")))
    t.check("a tree copy and its removal leave no trace", run("rm -r fs fd") == "" and not re.search(r"\bfs/|\bfd/", run("ls")))

    # ---- ls ----
    run("mkdir -p lr/s")
    run("write lr/x.txt xx")
    run("write lr/s/y.txt yy")
    out = run("ls lr/x.txt")
    t.check("ls of a file lists that file, by the name given", "lr/x.txt" in out and "lr:" not in out, out)
    out = run("ls lr/x.txt lr")
    t.check("files come first, then each directory under its name",
            out.index("lr/x.txt") < out.index("lr:") and "s/" in out, out)
    out = run("ls lr lr/s")
    t.check("two directories have headings and a blank between",
            "lr:" in out and "lr/s:" in out and "\n\nlr/s:" in out and "y.txt" in out, out)
    out = run("ls nope lr")
    t.check("a missing path is reported and the rest listed",
            "ls: nope: no such file" in out and "x.txt" in out and "[exit status 1]" in out, out)
    out = run("ls -R lr")
    t.check("ls -R goes down", "lr:" in out and "lr/s:" in out and "y.txt" in out and "x.txt" in out, out)
    t.check("ls -R into a pipe is only lines", run("ls -R lr | wc").split()[0] == "6")
    t.check("-l -1 -a -A are taken and change nothing",
            all(run("ls " + f + " lr") == run("ls lr") for f in ["-l", "-1", "-a", "-A", "-la"]))
    t.check("ls -R on a file is the file", "x.txt" in run("ls -R lr/x.txt") and "lr:" not in run("ls -R lr/x.txt"))
    t.check("an unknown ls option shows the usage", "usage: ls [-R]" in run("ls -Z"))
    run("rm -r lr")
    t.check("and it is gone", "lr/" not in run("ls"))


def tools_tests(m, t, url):
    """cat with several files, hexdump, and df."""
    def run(c, timeout=30):  # output as it is, indentation included
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    def counts(c):
        r = re.search(r"(\d+)\s+(\d+)\s+(\d+)", run(c, timeout=60))
        return tuple(int(x) for x in r.groups()) if r else None

    def shape(b):  # what wc says about these bytes
        return (b.count(b"\n"), len(b.split()), len(b))

    # ---- cat ----
    run("write ca.txt first")
    run("write cb.txt second")
    t.check("cat joins files", run("cat ca.txt cb.txt") == "first\nsecond")
    t.check("cat repeats a file named twice", run("cat ca.txt ca.txt") == "first\nfirst")
    out = run("cat ca.txt nope.txt cb.txt")
    t.check("a missing file is reported and the others still show",
            "first" in out and "nope.txt: no such file" in out and "second" in out and "[exit status 1]" in out, out)
    t.check("cat with no file and a terminal says how to use it", "usage: cat" in run("cat"))
    t.check("cat > file is a join", run("cat ca.txt cb.txt > cj.txt") == "" and run("cat cj.txt") == "first\nsecond")
    for f in ("rep.bin", "zero.bin"):
        out = run("fetch -o %s %s/%s" % (f, url, f), timeout=60)
        t.check("fetch %s (setting up)" % f, "saved" in out, out)
    t.check("cat into a pipe passes bytes exactly, joining a file with no final newline",
            run("cat rep.bin rep.bin | wc").split()[2] == str(2 * len(REP)))
    t.check("cat > file adds nothing either",
            run("cat rep.bin > rc.bin") == "" and run("wc rc.bin").split()[2] == str(len(REP)))
    t.check("cat joins big files (600000 bytes)",
            run("cat big.bin big.bin > two.bin", timeout=60) == "" and run("wc two.bin").split()[2] == "600000")

    # ---- hexdump ----
    def hd_c(data, start=0, squeeze=True, tail=True):
        rows, prev, star = [], None, False
        for i in range(0, len(data), 16):
            c = data[i:i + 16]
            if squeeze and len(c) == 16 and c == prev:
                if not star:
                    rows.append("*")
                star = True
                continue
            star = False
            prev = c if len(c) == 16 else None
            hx = lambda b: " ".join("%02x" % x for x in b)
            asc = "".join(chr(x) if 32 <= x < 127 else "." for x in c)
            rows.append("%08x  %-23s  %-23s  |%s|" % (start + i, hx(c[:8]), hx(c[8:]), asc))
        if tail and data:
            rows.append("%08x" % (start + len(data)))
        return "\n".join(rows)

    def hd_x(data, start=0, squeeze=True):
        rows, prev, star = [], None, False
        for i in range(0, len(data), 16):
            c = data[i:i + 16]
            if squeeze and len(c) == 16 and c == prev:
                if not star:
                    rows.append("*")
                star = True
                continue
            star = False
            prev = c if len(c) == 16 else None
            ws = [c[j] | (c[j + 1] << 8 if j + 1 < len(c) else 0) for j in range(0, len(c), 2)]
            rows.append("%07x" % (start + i) + "".join(" %04x" % w for w in ws))
        if data:
            rows.append("%07x" % (start + len(data)))
        return "\n".join(rows)

    t.check("hexdump -C: offsets, hex, text, repeated lines as *, the end offset",
            run("hexdump -C rep.bin") == hd_c(REP))
    t.check("-v shows every line", run("hexdump -vC rep.bin") == hd_c(REP, squeeze=False))
    t.check("hexdump (words, low byte first, odd byte last)", run("hexdump rep.bin") == hd_x(REP))
    t.check("a join of big files is right at the seam",
            run("hexdump -C -s 299992 -n 16 two.bin") == hd_c(BIG[-8:] + BIG[:8], 299992))
    run("rm two.bin")
    t.check("-x is the same", run("hexdump -x rep.bin") == run("hexdump rep.bin"))
    t.check("-v on words", run("hexdump -v rep.bin") == hd_x(REP, squeeze=False))
    t.check("a file of zeros: one line, a *, the end", run("hexdump -C zero.bin") == hd_c(bytes(64)) and
            run("hexdump -C zero.bin").split("\n")[1:] == ["*", "00000040"])
    t.check("-n stops after LENGTH bytes", run("hexdump -C -n 21 rep.bin") == hd_c(REP[:21]))
    t.check("-n 0x10 takes hex", run("hexdump -C -n 0x10 rep.bin") == hd_c(REP[:16]))
    t.check("-s starts at OFFSET and offsets count from the start of the file",
            run("hexdump -C -s 120 rep.bin") == hd_c(REP[120:], 120))
    t.check("-s and -n together, in words", run("hexdump -s 7 -n 9 rep.bin") == hd_x(REP[7:16], 7))
    t.check("options may be written joined to their values", run("hexdump -C -s100 -n20 rep.bin") == hd_c(REP[100:120], 100))
    t.check("-s past the end shows nothing", run("hexdump -s 1000 rep.bin") == "")
    t.check("an empty read shows nothing", run("hexdump -n 0 rep.bin") == "")
    t.check("two files are one stream", run("hexdump -C rep.bin rep.bin") == hd_c(REP + REP))
    t.check("-s counts through the first file to the next",
            run("hexdump -C -s 190 -n 16 rep.bin rep.bin") == hd_c((REP + REP)[190:206], 190))
    t.check("hexdump reads a pipe", run("cat rep.bin | hexdump -C -n 32") == hd_c(REP[:32]))
    t.check("and - means standard input", run("cat zero.bin | hexdump -C -") == hd_c(bytes(64)))
    t.check("-s on a pipe reads and drops the bytes", run("cat rep.bin | hexdump -C -s 130") == hd_c(REP[130:], 130))
    for a, n in [(0, 16), (1000, 50), (4095, 3), (4096, 4096), (299984, 16), (299990, 100)]:
        t.check("hexdump -C -s %d -n %d on a big file" % (a, n),
                run("hexdump -C -s %d -n %d big.bin" % (a, n)) == hd_c(BIG[a:a + n], a))
    out = run("hexdump -C big.bin | tail -n 3", timeout=120)
    t.check("a whole big file, read at the end", out == "\n".join(hd_c(BIG[299968:], 299968).split("\n")), out)
    t.check("and it is 18751 lines", run("hexdump -C big.bin | wc", timeout=120).split()[0] == "18751")
    out = run("hexdump -C -n 16 nope rep.bin")
    t.check("a missing file is reported and the next one dumped",
            "hexdump: nope: no such file" in out and hd_c(REP[:16]) in out, out)
    t.check("a bad length is refused", "invalid length: 'x'" in run("hexdump -n x rep.bin"))
    t.check("a bad option shows the usage", "usage: hexdump" in run("hexdump -Z rep.bin"))
    t.check("with no file and a terminal it shows the usage", "usage: hexdump" in run("hexdump"))

    # ---- df ----
    def df(c="df"):
        out = run(c).split("\n")
        return out[0], out[1].split() if len(out) > 1 else []

    head, row = df()
    t.check("df has a header and one row", head.split()[:4] == ["Filesystem", "1K-blocks", "Used", "Available"] and
            len(row) == 6 and row[5] == "/", f"{head!r} {row}")
    total, used, avail = int(row[1]), int(row[2]), int(row[3])
    t.check("used + available = size, and Use% rounds up", used + avail == total and
            row[4] == "%d%%" % -(-used * 100 // total), str(row))
    ls_tail = re.search(r"(\d+) KB free of (\d+) KB", run("ls"))
    t.check("df agrees with ls", ls_tail and (int(ls_tail.group(1)), int(ls_tail.group(2))) == (avail, total), str(row))
    t.check("df -k is the default", df("df -k") == (head, row))
    t.check("df FILE gives the same row", df("df cj.txt")[1] == row and df("df /")[1] == row)
    t.check("df of a missing file fails", "df: nope: no such file" in run("df nope"))
    t.check("df -h", re.fullmatch(r"disk\s+\d+[KMG]\s+\d+[KMG]\s+\d+[KMG]\s+\d+%\s+/", " ".join(run("df -h").split("\n")[1:])) is not None,
            run("df -h"))
    t.check("df -Z shows the usage", "usage: df" in run("df -Z"))
    run("cp big.bin big2.bin", timeout=60)
    _, row2 = df()
    t.check("a 300000-byte copy takes at least 293 KB", int(row2[3]) <= avail - 293 and int(row2[2]) >= used + 293, str(row2))
    run("rm big2.bin")
    t.check("deleting it gives the space back", df()[1][3] == row[3], str(df()[1]))

    for f in ("ca", "cb", "cj"):
        run("rm %s.txt" % f)
    for f in ("rep", "zero", "rc"):
        run("rm %s.bin" % f)


def find_tests(m, t, url):
    """find, against a small tree the test builds and a Python model of it."""
    def run(c, timeout=30):
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    def found(c):  # the lines of the output, order aside
        return sorted(l for l in run(c).split("\n") if l)

    dirs = ["ft", "ft/sub", "ft/sub/deep", "ft/empty", "ft/many"]
    files = {"ft/a.txt": 6, "ft/B.TXT": 4, "ft/zero": 0, "ft/sub/c.txt": 4, "ft/sub/z.log": 0,
             "ft/sub/deep/d.txt": 5, "ft/sub/deep/D2.dat": 9, "ft/big.bin": 300000}
    for d in dirs:
        run("mkdir " + d)
    for f, size in files.items():
        if f.endswith("big.bin"):
            run("cp big.bin " + f, timeout=60)
        elif size:
            run("write %s %s" % (f, "x" * (size - 1)))  # write adds the newline
        else:
            run("touch " + f)
    tree = {d: "d" for d in dirs}
    tree.update({f: "f" for f in files})

    def paths(pred=lambda p: True):
        return sorted(p for p in tree if pred(p))
    base = lambda p: p.rsplit("/", 1)[-1]
    depth = lambda p: p.count("/")
    low = lambda p: base(p).lower()

    t.check("find lists the tree, the starting point first", found("find ft") == paths() and run("find ft").split("\n")[0] == "ft")
    t.check("-type f", found("find ft -type f") == paths(lambda p: tree[p] == "f"))
    t.check("-type d", found("find ft -type d") == paths(lambda p: tree[p] == "d"))
    t.check("-name with a glob, ignoring case", found("find ft -name '*.txt'") == paths(lambda p: low(p).endswith(".txt")))
    t.check("-name matches the entry's name only", found("find ft -name sub") == ["ft/sub"])
    t.check("-name in capitals finds the lower-case name", found("find ft -name A.TXT") == ["ft/a.txt"])
    t.check("-iname is the same", found("find ft -iname 'b.txt'") == ["ft/B.TXT"])
    t.check("-name 'd*'", found("find ft -name 'd*'") == paths(lambda p: low(p).startswith("d")))
    t.check("-name '?.txt'", found("find ft -name '?.txt'") == paths(lambda p: re.fullmatch(r".\.txt", low(p)) is not None))
    t.check("-name '[a-c].txt'", found("find ft -name '[a-c].txt'") == ["ft/B.TXT", "ft/a.txt", "ft/sub/c.txt"])
    t.check("-name '[!a-c]*' with a negated class",
            found("find ft -type f -name '[!a-c]*'") == paths(lambda p: tree[p] == "f" and low(p)[0] not in "abc"))
    t.check("-path matches the whole path, * crossing /", found("find ft -path 'ft/sub/*'") ==
            paths(lambda p: p.startswith("ft/sub/")))
    t.check("-path '*deep*'", found("find ft -path '*deep*'") == paths(lambda p: "deep" in p))

    t.check("-maxdepth 0 is the starting point alone", found("find ft -maxdepth 0") == ["ft"])
    t.check("-maxdepth 1", found("find ft -maxdepth 1") == ["ft"] + paths(lambda p: depth(p) == 1))
    t.check("-mindepth 1 leaves the start out", found("find ft -mindepth 1") == paths(lambda p: p != "ft"))
    t.check("-mindepth 2 -maxdepth 2", found("find ft -mindepth 2 -maxdepth 2") == paths(lambda p: depth(p) == 2))
    t.check("an option may come after a test", found("find ft -type d -maxdepth 1") == ["ft"] + paths(lambda p: depth(p) == 1 and tree[p] == "d"))

    t.check("-size -1k -type f: rounded up, so only empty files", found("find ft -type f -size -1k") == ["ft/sub/z.log", "ft/zero"])
    t.check("-size +100k", found("find ft -size +100k") == ["ft/big.bin"])
    t.check("-size 293k rounds 292.97 up", found("find ft -size 293k") == ["ft/big.bin"] and found("find ft -size 292k") == [])
    t.check("-size 1M: anything from 1 byte to a megabyte rounds to 1",
            found("find ft -type f -size 1M") == paths(lambda p: tree[p] == "f" and files[p] > 0))
    t.check("-size 586 counts 512-byte blocks", found("find ft -size 586") == ["ft/big.bin"])
    t.check("-size 6c is exact", found("find ft -size 6c") == ["ft/a.txt"])
    t.check("-size +4c -size -9c", found("find ft -type f -size +4c -size -9c") == ["ft/a.txt", "ft/sub/deep/d.txt"])
    t.check("-size 1 is one block", found("find ft -type f -size 1") == paths(lambda p: tree[p] == "f" and 0 < files[p] <= 512))

    t.check("-empty", found("find ft -empty") == ["ft/empty", "ft/many", "ft/sub/z.log", "ft/zero"])
    t.check("-empty -type d", found("find ft -type d -empty") == ["ft/empty", "ft/many"])

    t.check("! -type d", found("find ft ! -type d") == paths(lambda p: tree[p] == "f"))
    t.check("-not -name", found("find ft -type f -not -name '*.txt'") ==
            paths(lambda p: tree[p] == "f" and not low(p).endswith(".txt")))
    t.check("-o", found("find ft -name '*.log' -o -name zero") == ["ft/sub/z.log", "ft/zero"])
    t.check("-a binds tighter than -o", found("find ft -name '*.log' -o -name '*.txt' -a -path 'ft/sub/*'") ==
            ["ft/sub/c.txt", "ft/sub/deep/d.txt", "ft/sub/z.log"])
    t.check("parentheses", found("find ft ( -name a.txt -o -name z.log ) -type f") == ["ft/a.txt", "ft/sub/z.log"])
    t.check("and written \\( \\) as well", found("find ft \\( -name a.txt -o -name z.log \\) -type f") == ["ft/a.txt", "ft/sub/z.log"])
    t.check("-and, -or and -not spelled out", found("find ft -type f -and -not -name '*.txt' -and -size -1k") == ["ft/sub/z.log", "ft/zero"])

    out = run("find ft/sub ft/empty -maxdepth 0").split("\n")
    t.check("several starting points come in the order given", out == ["ft/sub", "ft/empty"], str(out))
    t.check("a trailing slash is kept on the start and not doubled", found("find ft/ -maxdepth 1") ==
            ["ft/"] + paths(lambda p: depth(p) == 1))
    run("cd ft")
    t.check("no path means the current directory", found("find -maxdepth 1") == sorted(["."] + ["./" + base(p) for p in paths(lambda p: depth(p) == 1)]))
    t.check("find . names the current directory .", found("find . -name . -maxdepth 0") == ["."])
    run("cd ..")
    t.check("-print0 writes the same bytes with NULs for newlines",
            run("find ft -type f -print0 | wc").split()[2] == run("find ft -type f | wc").split()[2])
    t.check("-print more than once", sorted(run("find ft -name zero -print -print").split("\n")) == ["ft/zero", "ft/zero"])
    t.check("a test with an action does not also print", found("find ft -name zero -print -o -name a.txt") == ["ft/zero"])

    out = run("find ft -depth").split("\n")
    t.check("-depth lists a directory after what is in it",
            out[-1] == "ft" and out.index("ft/sub") > out.index("ft/sub/deep/d.txt") and out.index("ft/sub/deep") > out.index("ft/sub/deep/D2.dat"), str(out))
    t.check("-depth is still the whole tree", sorted(out) == paths())

    t.check("-exec runs a program with {} replaced", run("find ft -name a.txt -exec head -n 1 {} ;") == "xxxxx")
    t.check("-exec in the middle of an argument", run("find ft -name a.txt -exec head -c 3 {} ;") == "xxx")
    out = run("find ft -type f -name '*.txt' -exec head -n 1 {} ;").split("\n")
    t.check("-exec runs once per match", sorted(out) == sorted("x" * (files[f] - 1) for f in files if f.lower().endswith(".txt")), str(out))
    out = run("find ft -type f -name '*.txt' -exec grep xxxx {} ; -print").split("\n")
    t.check("-exec that succeeds is true, so -print follows",
            "ft/a.txt" in out and "ft/sub/deep/d.txt" in out and "ft/B.TXT" not in out and "ft/sub/c.txt" not in out, str(out))
    out = run("find ft -type f -name '*.txt' -exec grep zzz {} ; -print")
    t.check("-exec that fails is false, so nothing prints", out == "", out)
    t.check("-exec runs cat", run("find ft -name a.txt -exec cat {} ;") == "xxxxx")
    out = run("find ft -name a.txt -exec pwd {} ;")
    t.check("a built-in cannot be exec'd, and the message says why", "pwd: no such file" in out and "built-ins" in out, out)
    out = run("find ft -name a.txt -exec wc {} ;")
    t.check("another program works too", re.search(r"1\s+1\s+6 ft/a.txt", out) is not None, out)

    # errors
    out = run("find nope")
    t.check("a missing starting point", "find: nope: no such file" in out and "[exit status 1]" in out, out)
    out = run("find nope ft -name a.txt")
    t.check("and the rest are still searched", "find: nope:" in out and "ft/a.txt" in out, out)
    for c, what in [("find ft -foo", "unknown predicate '-foo'"), ("find ft -name", "missing argument to '-name'"),
                    ("find ft -type x", "unknown argument 'x' to '-type'"), ("find ft -size 3x", "invalid argument '3x' to '-size'"),
                    ("find ft -size", "missing argument to '-size'"), ("find ft -maxdepth z", "invalid argument 'z' to '-maxdepth'"),
                    ("find ft -exec wc", "missing argument to '-exec'"), ("find ft ( -name a", "no matching ')'"),
                    ("find ft -name a )", "unexpected ')'"), ("find ft -name a -o", "ends where a test was expected")]:
        out = run(c)
        t.check(c + " is refused", what in out and "[exit status 2]" in out, out)
    t.check("a refused expression searches nothing", "ft/" not in run("find ft -foo"))

    # a long chain and a wide directory
    chain = "ft/" + "/".join("lvl%d" % i for i in range(12))
    acc = "ft"
    for i in range(12):
        acc += "/lvl%d" % i
        run("mkdir " + acc)
    run("write %s/bottom.txt deep" % chain)
    t.check("a twelve-level tree", found("find ft -name bottom.txt") == [chain + "/bottom.txt"])
    t.check("-mindepth reaches the bottom", found("find ft -mindepth 13") == [chain + "/bottom.txt"])
    for i in range(40):
        run("touch ft/many/f%02d.tmp" % i)
    t.check("a directory of 40 entries (more than the first batch)",
            found("find ft/many -type f") == ["ft/many/f%02d.tmp" % i for i in range(40)])

    # -delete: all matches in a directory, in the right order, nothing else
    t.check("-delete removes 40 files in one directory", run("find ft/many -name '*.tmp' -delete") == "" and found("find ft/many") == ["ft/many"])
    t.check("-delete removes only what matched", run("find ft/sub -name '*.txt' -delete") == "" and
            found("find ft/sub") == ["ft/sub", "ft/sub/deep", "ft/sub/deep/D2.dat", "ft/sub/z.log"])
    out = run("find ft -maxdepth 0 -delete")
    t.check("-delete of a directory with things in it fails and says so", "cannot delete ft" in out and "[exit status 1]" in out, out)
    t.check("and leaves everything", "ft/a.txt" in found("find ft"))
    t.check("-delete takes a whole subtree, deepest first", run("find ft/sub -delete") == "" and "no such file" in run("find ft/sub"))
    t.check("-delete -type d -empty", run("find ft -type d -empty -delete") == "" and "ft/empty" not in found("find ft") and "ft/many" not in found("find ft"))
    t.check("what was not matched survives", "ft/a.txt" in found("find ft") and "ft/big.bin" in found("find ft"))
    t.check("-delete on the lot", run("find ft -delete") == "" and "no such file" in run("find ft"))


def gnu_text_tests(m, t):
    """v0.24.9 on: the text tools against what GNU coreutils print. The
    commands and the expected output come from tests/text_cases.py, made
    by tests/gen_text_cases.py running the real tools."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from text_cases import CASES, ERRS

    def run(c, timeout=30):
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "")

    for n, (cmds, expected) in enumerate(CASES):
        run("mkdir tcdir")
        run("cd tcdir")
        got = [run(c.lstrip("?")).rstrip("\n") for c in cmds]
        run("cd ..")
        run("rm -r tcdir")
        want = [e.rstrip("\n") for e in expected]
        t.check(f"text case {n}: {' ; '.join(cmds)[:70]}", got == want, f"got {got!r} expected {want!r}")

    # ---- the things GNU words differently (messages) or that need a status ----
    def st(c):  # (output, exit status): the shell prints [exit status N] for a failure
        out = run(c).rstrip("\n")
        mm = re.search(r"\n?\[exit status (\d+)\]$", out)
        return (out[:mm.start()] if mm else out), (int(mm.group(1)) if mm else 0)

    run("echo -e 'one\\ntwo' > gt.txt")
    out, rc = st("cut")
    t.check("cut with nothing to do says so", "you must specify a list" in out and "usage: cut" in out and rc == 2, out)
    out, rc = st("cut -f 0 gt.txt")
    t.check("cut counts from 1", "numbered from 1" in out and rc == 2, out)
    out, rc = st("cut -f 3-1 gt.txt")
    t.check("cut refuses a decreasing range", "decreasing range" in out and rc == 2, out)
    out, rc = st("cut -d ab -f 1 gt.txt")
    t.check("cut wants a one-character delimiter", "single character" in out and rc == 2, out)
    out, rc = st("cut -s -c 1 gt.txt")
    t.check("cut -s only makes sense with -f", "only when operating on fields" in out and rc == 2, out)
    out, rc = st("cut -c 1,x gt.txt")
    t.check("cut refuses a bad list", "invalid" in out and rc == 2, out)
    out, rc = st("cut -c 1 nosuch gt.txt")
    t.check("a missing file is reported and the rest cut", "nosuch: no such file" in out and out.endswith("o\nt") and rc == 1, out)
    out, rc = st("tr a")
    t.check("tr a alone wants a second set", "missing operand" in out and "Two strings" in out and rc == 2, out)
    out, rc = st("tr")
    t.check("tr with nothing says so", "missing operand" in out and rc == 2, out)
    out, rc = st("echo x | tr -d a b")
    t.check("tr -d takes one set", "extra operand" in out and rc == 2, out)
    out, rc = st("echo x | tr z-a x")
    t.check("tr refuses a reversed range", "reverse collating" in out and rc == 2, out)
    out, rc = st("echo x | tr '[:foo:]' x")
    t.check("tr refuses an unknown class", "invalid character class" in out and rc == 2, out)
    out, rc = st("echo x | tr -q a b")
    t.check("tr shows the usage for a bad option", "usage: tr" in out and rc == 2, out)
    out, rc = st("echo hi | tee nodir/x")
    t.check("tee reports a file it cannot open but still copies", out.startswith("tee: nodir/x:") and out.endswith("hi") and rc == 1, out)
    out, rc = st("tee -z")
    t.check("tee shows the usage for a bad option", "usage: tee" in out and rc == 2, out)
    # a pipeline that is waiting must not hold output back
    t.check("tee passes lines on as they come", run("echo -e 'a\\nb' | tee tl.txt | head -n 1").rstrip("\n") == "a" and run("cat tl.txt").rstrip("\n") == "a\nb")
    run("rm tl.txt")
    run("rm gt.txt")

    # ---- v0.24.10: the small tools ----
    out, rc = st("true")
    t.check("true succeeds quietly", out == "" and rc == 0, out)
    out, rc = st("false")
    t.check("false fails quietly", out == "" and rc == 1, out)
    t.check("yes says y until the reader leaves", run("yes | head -n 3").replace("\n", " ").strip() == "y y y")
    t.check("yes joins its arguments", run("yes a b | head -n 2").replace("\n", " ").strip() == "a b a b")
    t.check("yes fills a pipe past one buffer", run("yes | head -n 1000 | wc -l").strip() == "1000")
    out, rc = st("seq")
    t.check("seq with no operand says so", "missing operand" in out and "usage: seq" in out and rc == 2, out)
    out, rc = st("seq 1 0 5")
    t.check("seq refuses a zero step", "zero increment" in out and rc == 1, out)
    out, rc = st("seq 1 x")
    t.check("seq refuses a word", "invalid floating point argument: 'x'" in out and rc == 1, out)
    out, rc = st("seq 1 2 3 4")
    t.check("seq refuses a fourth operand", "extra operand '4'" in out and rc == 2, out)
    out, rc = st("seq -x 3")
    t.check("seq shows the usage for a bad option", "usage: seq" in out and rc == 2, out)
    t.check("seq counts big and does not stall", run("seq 100000 | tail -n 1").strip() == "100000")
    out, rc = st("basename")
    t.check("basename with no operand says so", "missing operand" in out and rc == 2, out)
    out, rc = st("basename a b c")
    t.check("basename refuses three names", "extra operand 'c'" in out and rc == 2, out)
    out, rc = st("dirname")
    t.check("dirname with no operand says so", "missing operand" in out and rc == 2, out)
    run("echo -e 'abc' > c1")
    run("echo -e 'abc\\nx\\c' > c2")
    out, rc = st("cmp c1 c2")
    t.check("cmp reports a file that ends first, on stderr", out == "cmp: EOF on c1 after byte 4" and rc == 1, out)
    out, rc = st("cmp c2 c1")
    t.check("cmp names the shorter one", out == "cmp: EOF on c1 after byte 4" and rc == 1, out)
    out, rc = st("cmp -s c1 c2")
    t.check("cmp -s says nothing", out == "" and rc == 1, out)
    out, rc = st("cmp c1 nosuch")
    t.check("cmp reports a missing file with status 2", "nosuch: no such file" in out and rc == 2, out)
    out, rc = st("echo -e 'abc' | cmp c1")
    t.check("cmp with one file reads standard input for the other", out == "" and rc == 0, out)
    out, rc = st("cmp")
    t.check("cmp with nothing says so", "missing operand" in out and rc == 2, out)
    out, rc = st("cmp -n x c1 c2")
    t.check("cmp -n wants a number", "invalid" in out and rc == 2, out)
    run("rm c1")
    run("rm c2")

    # ---- sed: what GNU words differently, and what the generated cases cannot show ----
    run("echo -e 'one\\ntwo' > gt.txt")
    bad = [sc for sc, want in ERRS if st("echo a | sed -n '" + sc + "'")[1] != want]
    t.check(f"sed gives GNU's exit status for {len(ERRS)} scripts, good and bad", not bad, f"{bad}")
    silent = [sc for sc, want in ERRS if want and not st("echo a | sed -n '" + sc + "'")[0].startswith("sed:")]
    t.check("every sed script error says so", not silent, f"{silent}")
    out, rc = st("sed")
    t.check("sed with nothing to do prints the usage", "usage: sed" in out and rc == 1, out)
    out, rc = st("sed --bogus p")
    t.check("sed refuses an unknown option", "unrecognized option" in out and rc == 1, out)
    out, rc = st("sed k gt.txt")
    t.check("sed names an unknown command", "unknown command: `k'" in out and rc == 1, out)
    out, rc = st("sed -n p nosuch gt.txt")
    t.check("sed reports a missing file, reads on, and ends with status 2", "can't read nosuch" in out and out.endswith("one\ntwo") and rc == 2, out)
    out, rc = st("sed -i p")
    t.check("sed -i wants files", "no input files" in out and rc == 4, out)
    out, rc = st("sed 's/\\(a\\)\\1/x/' gt.txt")
    t.check("a back-reference in a pattern is refused, not misread", "back-reference" in out and rc == 1, out)
    out, rc = st("sed -n '/x/b nolabel' gt.txt")
    t.check("sed names a missing label, status 4", "can't find label" in out and rc == 4, out)
    out, rc = st("sed -n p -l x gt.txt")
    t.check("sed -l wants a number", "invalid line length" in out and rc == 1, out)
    out, rc = st("sed -i.orig 's/one/1/' gt.txt")
    t.check("sed -i with a suffix keeps the original", out == "" and run("cat gt.txt").rstrip("\n") == "1\ntwo" and run("cat gt.txt.orig").rstrip("\n") == "one\ntwo", out)
    run("rm gt.txt.orig")
    out, rc = st("sed -n p gt.txt gt.txt")
    t.check("sed joins its files into one stream", out == "1\ntwo\n1\ntwo" and rc == 0, out)
    run("rm gt.txt")

    # ---- diff: what the generated cases cannot show (the core itself is
    # checked against GNU diff by tests/test_diff.c) ----
    run("echo -e 'one\\ntwo\\nthree' > da")
    run("echo -e 'one\\n2\\nthree' > db")
    stamp = r"\t20\d\d-[01]\d-[0-3]\d [0-2]\d:[0-5]\d:[0-5]\d\.000000000 \+0000\n"
    out, rc = st("diff -u da db")
    t.check("diff -u heads each file with its name and when it was written",
            re.match(r"--- da" + stamp + r"\+\+\+ db" + stamp + "@@ -1,3 \\+1,3 @@\n one\n-two\n\\+2\n three$", out) is not None and rc == 1, out)
    out, rc = st("diff -c da db")
    cstamp = r"\t(Mon|Tue|Wed|Thu|Fri|Sat|Sun) (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 1-3]\d [0-2]\d:[0-5]\d:[0-5]\d 20\d\d\n"
    t.check("diff -c heads them the older way", re.match(r"\*\*\* da" + cstamp + "--- db" + cstamp + r"\*{15}\n", out) is not None and rc == 1, out)
    out, rc = st("cat da | diff -u - db")
    t.check("standard input is stamped with the time now", re.match(r"--- -" + stamp + r"\+\+\+ db" + stamp, out) is not None and rc == 1, out)
    out, rc = st("diff -u -N da nosuch")
    t.check("diff -N stamps a missing file 1970", "+++ nosuch\t1970-01-01 00:00:00.000000000 +0000\n@@ -1,3 +0,0 @@" in out and rc == 1, out)
    out, rc = st("echo x | diff - -")
    t.check("diff - - reads standard input once", out == "" and rc == 0, out)
    out, rc = st("diff nosuch da")
    t.check("diff reports a missing file with status 2", out == "diff: nosuch: no such file or directory" and rc == 2, out)
    out, rc = st("diff da")
    t.check("diff with one file says so", "missing operand after 'da'" in out and rc == 2, out)
    out, rc = st("diff -u -c da db")
    t.check("diff takes one output style", "conflicting output style options" in out and rc == 2, out)
    out, rc = st("diff -U x da db")
    t.check("diff -U wants a number", "invalid context length 'x'" in out and rc == 2, out)
    out, rc = st("diff --bogus da db")
    t.check("diff refuses an unknown option", "unrecognized option '--bogus'" in out and rc == 2, out)
    run("mkdir dd")
    out, rc = st("cat da | diff - dd")
    t.check("standard input is not compared with a directory", "cannot compare '-' to a directory" in out and rc == 2, out)
    run("rmdir dd")
    run("seq 1 5000 > big1")
    run("seq 1 5000 | sed s/9/N/ > big2")
    t.check("diff gets through 5000 lines with 1760 changes", run("diff big1 big2 | wc -l", timeout=120).strip() == "3520")
    for f in ["da", "db", "big1", "big2"]:
        run("rm " + f)

    # ---- v0.29.0: tac, rev and nl: statuses and messages, and what the generated cases cannot show ----
    run("echo -e 'a\\nb\\n\\c' > nf1")
    out, rc = st("tac nosuch nf1")
    t.check("tac reports a missing file and goes on", out == "tac: nosuch: no such file or directory\nb\na" and rc == 1, out)
    out, rc = st("echo -e 'x\\ny' | tac nf1 -")
    t.check("tac takes - for standard input", out == "b\na\ny\nx" and rc == 0, out)
    out, rc = st("tac -q nf1")
    t.check("tac shows the usage for a bad option", "usage: tac" in out and rc == 2, out)
    out, rc = st("tac -s")
    t.check("tac -s wants its separator", "usage: tac" in out and rc == 2, out)
    out, rc = st("tac")
    t.check("tac with only the keyboard to read says how to use it", "usage: tac" in out and rc == 2, out)
    run("seq 1 20000 > nbig")
    t.check("tac reads a file bigger than its first buffer",
            run("tac nbig | head -n 2").replace("\n", " ").strip() == "20000 19999" and run("tac nbig | tail -n 1").strip() == "1"
            and run("tac nbig | wc -l").strip() == "20000", run("tac nbig | wc -l"))
    t.check("tac -s ' ' on one long line", run("echo -e '1 2 3 4 5\\c' | tac -s ' '").strip() == "54 3 2 1")
    run("rm nbig")
    # the prompt starts on a fresh line even when the last line has no newline
    m.send("echo -n abc | tac\r")
    raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
    t.check("tac ends the screen output with a newline", raw.endswith("abc\nringtail:/> "), repr(raw))
    m.send("echo -n abc | rev\r")
    raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
    t.check("rev ends the screen output with a newline", raw.endswith("cba\nringtail:/> "), repr(raw))
    out, rc = st("echo abc | rev nosuch - nf1")
    t.check("rev reports a missing file, reads - and goes on", out == "rev: nosuch: no such file or directory\ncba\na\nb" and rc == 1, out)
    out, rc = st("rev -q")
    t.check("rev shows the usage for a bad option", "usage: rev" in out and rc == 2, out)
    out, rc = st("rev")
    t.check("rev with only the keyboard to read says how to use it", "usage: rev" in out and rc == 2, out)
    for opt, what in [("-b x", "invalid body numbering style: 'x'"), ("-h x", "invalid header numbering style: 'x'"), ("-f ''", "invalid footer numbering style: ''"),
                      ("-n xx", "invalid line numbering format: 'xx'"), ("-n r", "invalid line numbering format: 'r'"), ("-w 0", "invalid line number field width: '0'"),
                      ("-w -1", "invalid line number field width: '-1'"), ("-w 2147483648", "invalid line number field width: '2147483648'"),
                      ("-w 3x", "invalid line number field width: '3x'"), ("-w 0x3", "invalid line number field width: '0x3'"),
                      ("-l 0", "invalid line number of blank lines: '0'"), ("-v x", "invalid starting line number: 'x'"),
                      ("-v 9223372036854775808", "invalid starting line number: '9223372036854775808'"), ("-v ''", "invalid starting line number: ''"),
                      ("-i 1.5", "invalid line number increment: '1.5'"), ("-i -9223372036854775809", "invalid line number increment: '-9223372036854775809'"),
                      ("-b 'p\\('", "invalid regular expression")]:
        out, rc = st(f"nl {opt} nf1")
        t.check(f"nl {opt} is refused with status 1", out.startswith("nl: " + what) and rc == 1, out)
    out, rc = st("nl nosuch nf1")
    t.check("nl reports a missing file and goes on", out == "nl: nosuch: no such file or directory\n     1\ta\n     2\tb" and rc == 1, out)
    out, rc = st("nl -X nf1")
    t.check("nl shows the usage for a bad option", "usage: nl" in out and rc == 2, out)
    out, rc = st("nl nf1 -w")
    t.check("nl -w wants a width", "usage: nl" in out and rc == 2, out)
    out, rc = st("nl")
    t.check("nl with only the keyboard to read says how to use it", "usage: nl" in out and rc == 2, out)
    out, rc = st("echo -e 'a\\nb' | nl -v 9223372036854775807")
    t.check("nl stops when the numbers run out, after the lines it could number",
            out == "9223372036854775807\ta\nnl: line number overflow" and rc == 1, out)
    out, rc = st("echo -e 'a\\n\\\\:\\\\:\\nb\\nc' | nl -p -v 9223372036854775807")
    t.check("-p keeps the numbers running out across a page",
            out == "9223372036854775807\ta\n\nnl: line number overflow" and rc == 1, out)
    out, rc = st("echo -e 'a\\nb' | nl -v -9223372036854775807 -i -2")
    t.check("and the same downwards", out == "-9223372036854775807\ta\nnl: line number overflow" and rc == 1, out)
    out, rc = st("echo x | nl -w 200 | wc -c")
    t.check("a wide number field is as wide as asked", out.strip() == "203" and rc == 0, out)
    run("rm nf1")

    # ---- v0.29.0: paste and comm: the usage errors, and what the generated cases cannot show ----
    run("echo -e 'a\\nb\\n\\c' > pf1")
    run("echo -e 'b\\nc\\n\\c' > pf2")
    for c, what in [("paste -q pf1", "usage: paste"), ("paste -d", "usage: paste"), ("paste -s -d", "usage: paste"), ("paste", "usage: paste")]:
        out, rc = st(c)
        t.check(f"{c} shows the usage", what in out and rc == 2, out)
    out, rc = st("paste pf1 nosuch pf2")
    t.check("paste with a missing file prints nothing and says so", out == "paste: nosuch: no such file or directory" and rc == 1, out)
    run("mkdir pdir")
    out, rc = st("paste pdir")
    t.check("paste of a directory is an error", out.startswith("paste: pdir: ") and rc == 1, out)
    for c in ["comm", "comm pf1", "comm pf1 pf2 pf1", "comm -x pf1 pf2", "comm -4 pf1 pf2", "comm --foo pf1 pf2",
              "comm --output-delimiter pf1 pf2", "comm --total=1 pf1 pf2", "comm --check-order=1 pf1 pf2", "comm --chex pf1 pf2", "comm --tx pf1 pf2",
              "comm --checkorder pf1 pf2"]:
        out, rc = st(c)
        t.check(f"{c} shows the usage", "usage: comm" in out and rc == 2, out)
    out, rc = st("echo x | comm - -")
    t.check("comm will not read standard input twice", out.startswith("comm: only one of the files") and rc == 1, out)
    out, rc = st("comm pdir pf1")
    t.check("comm of a directory is an error", out.startswith("comm: pdir: ") and rc == 1, out)
    out, rc = st("comm pf1 nosuch")
    t.check("comm with a missing file prints nothing and says so", out == "comm: nosuch: no such file or directory" and rc == 1, out)
    run("rmdir pdir")
    # lines longer than the read buffer
    run("seq 1 1000 | tr '\\n' ' ' > plong")
    n = int(run("wc -c plong").split()[0])
    t.check("paste of a line longer than its buffer", run("paste plong plong | wc -c").strip() == str(2 * n + 2), run("paste plong plong | wc -c"))
    t.check("paste -s of long lines", run("paste -s plong plong | wc -c").strip() == str(2 * n + 2), run("paste -s plong plong | wc -c"))
    t.check("comm of a line longer than its buffer", run("comm plong plong | wc -c").strip() == str(n + 3), run("comm plong plong | wc -c"))
    out, rc = st("comm -23 plong pf1 | wc -c")
    t.check("and when it is the only one", out.strip() == str(n + 1) and rc == 0, out)
    run("rm plong pf1 pf2")

    # ---- v0.30.0: expand, unexpand and fold: the usage errors, the screen, and what the generated cases cannot show ----
    run("echo -e 'a\\tb\\n\\c' > xf")
    for prog, usage, bad in [
            ("expand", "usage: expand", ["-x", "--nosuch", "--tabs", "-t", "--init=3", "-z", "-ix", "-t 4 -z", "--tab"]),
            ("unexpand", "usage: unexpand", ["-x", "--nosuch", "--tabs", "-t", "--all=3", "-z", "-ax", "--first-only=1", "--f=1"]),
            ("fold", "usage: fold", ["-x", "--nosuch", "--width", "-w", "--bytes=3", "-z", "-bw", "-bx", "--spaces=1"])]:
        for b in bad:
            out, rc = st(f"echo x | {prog} {b}")
            t.check(f"{prog} {b} shows the usage", usage in out and rc == 2, out)
        out, rc = st(prog)
        t.check(f"{prog} with only the keyboard to read says how to use it", usage in out and rc == 2, out)
    # a bad -t list is refused before anything is read
    out, rc = st("expand -t 3,2")
    t.check("expand checks the list before the keyboard", out == "expand: tab sizes must be ascending" and rc == 1, out)
    out, rc = st("unexpand -t 0")
    t.check("unexpand checks the list before the keyboard", out == "unexpand: tab size cannot be 0" and rc == 1, out)
    out, rc = st("fold -w 0")
    t.check("fold checks the width before the keyboard", out.startswith("fold: invalid number of columns: '0'") and rc == 1, out)
    # the prompt starts on a fresh line even when the last line has no newline
    for c, want in [("echo -e 'a\\tb\\c' | expand", "a       b\nringtail:/> "), ("echo -e '        x\\c' | unexpand", "\tx\nringtail:/> "),
                    ("echo -e 'abcdef\\c' | fold -w 4", "abcd\nef\nringtail:/> ")]:
        m.send(c + "\r")
        raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
        t.check(f"{c.split('|')[1].split()[0]} ends the screen output with a newline", raw.endswith(want), repr(raw))
    # a file bigger than the read buffer, and a line that is
    run("seq 1 20000 | paste - - - - > xbig")
    t.check("expand then unexpand gives the file back", run("expand xbig | unexpand -a | cmp - xbig").strip() == "" and run("expand xbig | wc -l").strip() == "5000")
    nbytes = int(run("wc -c xbig").split()[0])
    t.check("fold -w 1 makes a line of each byte", run("fold -w 1 xbig | wc -l").strip() == str(nbytes - 5000), run("fold -w 1 xbig | wc -l"))
    run("rm xbig xf")

    # ---- v0.31.0: od and strings: the usage errors, the screen, and what the generated cases cannot show ----
    run("echo -e 'hello\\n\\x00\\x01world' > of")
    for prog, usage, bad in [
            ("od", "usage: od", ["-z", "--nosuch", "-A", "-t", "-j", "-N", "-A q", "-t q", "-t a1", "--format", "-x -z", "--width=-1", "-w0"]),
            ("strings", "usage: strings", ["-z", "--nosuch", "-n", "-t", "-e", "--bytes", "-t q", "-e x", "--radix=q", "--encoding=x", "-ax", "-fz"])]:
        for b in bad:
            out, rc = st(f"echo x | {prog} {b}")
            t.check(f"{prog} {b} complains", rc != 0 and out != "", out)
            if prog == "strings" or b in ("-z", "--nosuch", "-A", "-t", "-j", "-N", "-x -z", "--format"):
                t.check(f"{prog} {b} shows the usage", usage in out and rc == 2, out)
        out, rc = st(prog)
        t.check(f"{prog} with only the keyboard to read says how to use it", usage in out and rc == 2, out)
    out, rc = st("strings -")
    t.check("strings - alone is no way to name the input", "usage: strings" in out and rc == 2, out)
    out, rc = st("echo x | strings -")
    t.check("strings - from a pipe is no way to name the input either", "usage: strings" in out and rc == 2, out)
    out, rc = st("od --nosuch of")
    t.check("od does not take the file for the value of an option it does not know", "usage: od" in out and rc == 2, out)
    out, rc = st("echo x | od --output-duplicates=1")
    t.check("od --output-duplicates takes no value", "usage: od" in out and rc == 2, out)
    for w in ["-w0", "--width=0", "-w00"]:
        out, rc = st(f"od {w} of")
        t.check(f"od {w} is refused", out.startswith("od: invalid") and "argument '0" in out and rc == 1, out)
    out, rc = st("od -w65538 of")
    t.check("od -w65538 is more than it will do", out == "od: -w argument too large (at most 65536)" and rc == 1, out)
    out, rc = st("od -w65536 -t x1 of")
    t.check("od -w65536 is the most it will do", out.startswith("0000000 68 65 6c 6c 6f") and out.endswith("0000016") and rc == 0, out)
    out, rc = st("echo x | od -t f4")
    t.check("od says it has no floating-point types", "floating" in out and rc == 2, out)
    out, rc = st("echo x | od -t fF")
    t.check("od -t fF is refused too", "floating" in out and rc == 2, out)
    # a read that fails: a directory
    run("mkdir odd")
    out, rc = st("od odd")
    t.check("od of a directory fails", rc == 1 and out.startswith("od: odd:") and "directory" in out, out)
    out, rc = st("od of odd of")
    t.check("od of a directory in the middle says so and goes on", rc == 1 and out.startswith("od: odd: is a directory\n0000000 062550 066154 005157 000400 067567 066162 005144 062550\n") and out.endswith("0000034"), out)
    out, rc = st("od -j 1 odd of")
    t.check("a directory is no use to skip over", rc == 1 and out == "od: odd: is a directory\n0000001 066145 067554 000012 073401 071157 062154 000012\n0000016", out)
    run("rmdir odd")
    # the prompt starts on a fresh line, and the listing ends on its own line
    for c, want in [("echo -e 'abcd\\c' | strings", "abcd\nringtail:/> "), ("echo -e 'abcd\\c' | od -c", "0000004\nringtail:/> ")]:
        m.send(c + "\r")
        raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
        t.check(f"{c.split('|')[1].split()[0]} ends the screen output with a newline", raw.endswith(want), repr(raw))
    # files bigger than the read buffers, and the same bytes from a pipe
    run("seq 1 20000 > ob")
    nbytes = int(run("wc -c ob").split()[0])
    t.check("od -w1 gives one line to a byte", run("od -v -An -t u1 -w1 ob | wc -l").strip() == str(nbytes), run("od -v -An -t u1 -w1 ob | wc -l"))
    t.check("od counts the whole file", run("od -A d ob | tail -n 1").strip() == f"{nbytes:07d}", run("od -A d ob | tail -n 1"))
    t.check("strings finds every number", run("strings -n 1 ob | wc -l").strip() == "20000", run("strings -n 1 ob | wc -l"))
    t.check("strings -n 5 finds the five-digit ones", run("strings -n 5 ob | wc -l").strip() == "10001", run("strings -n 5 ob | wc -l"))
    t.check("strings -t d gives the place of the first one", run("strings -n 5 -t d ob | head -n 1").strip() == "48888 10000", run("strings -n 5 -t d ob | head -n 1"))
    t.check("strings -t d with a file name", run("strings -f -t d -n 5 ob | head -n 1").strip() == "ob:   48888 10000", run("strings -f -t d -n 5 ob | head -n 1"))
    for off in ["0", "1", "1023", "1024", "4095", "4096", "65535", "65536", "100000"]:
        a = run(f"od -j {off} -N 40 -c ob")
        b = run(f"seq 1 20000 | od -j {off} -N 40 -c")
        t.check(f"od -j {off} reads a pipe the way it reads a file", a == b and a.strip() != "", f"{a!r} {b!r}")
    t.check("a line longer than the buffer", run("seq 1 20000 | paste -s -d x | strings | wc -c").strip() == str(nbytes), run("seq 1 20000 | paste -s -d x | strings | wc -c"))
    t.check("the same through a pipe", run("cat ob ob | od -A d -j 100000 | tail -n 1").strip() == f"{2 * nbytes:07d}", run("cat ob ob | od -A d -j 100000 | tail -n 1"))
    t.check("od agrees with hexdump on what is in the file", run("od -An -tx2 -v ob | tr -d ' \\n' | head -c 200").strip() == run("hexdump -v ob | cut -c 9- | tr -d ' \\n' | head -c 200").strip())
    # hexdump still reads the same way now it shares the reader with od
    t.check("hexdump -s and -n still skip and stop", run("hexdump -s 1000 -n 16 ob") == "00003e8 3732 0a38 3732 0a39 3832 0a30 3832 0a31\n00003f8\n", run("hexdump -s 1000 -n 16 ob"))
    t.check("od agrees with hexdump on the words", run("od -A x -j 1000 -N 16 -t x2 ob").replace("0003e8", "00003e8").replace("0003f8", "00003f8") == run("hexdump -s 1000 -n 16 ob"))
    run("rm ob of")

    # ---- v0.32.0: xargs: the limits of spawn, the exit statuses, and what the generated cases cannot show ----
    def rr(c):
        return run(c).rstrip("\n")

    # a run holds 64 words, the command among them
    lines = rr("seq 1 100 | xargs echo").split("\n")
    t.check("xargs cuts a run at 64 words", len(lines) == 2 and len(lines[0].split()) == 63 and lines[1].split()[0] == "64" and lines[1].split()[-1] == "100", lines)
    lines = rr("seq 1 100 | xargs echo a b").split("\n")
    t.check("the command's own words count against the 64", len(lines) == 2 and len(lines[0].split()) == 63 and lines[0].endswith(" 61") and lines[1].startswith("a b 62 63"), lines)
    lines = rr("seq 1 100 | xargs -n 70 echo").split("\n")
    t.check("-n larger than a run holds is cut to what it holds", len(lines) == 2 and len(lines[0].split()) == 63, lines)
    t.check("every item turns up once, in order", run("seq 1 100 | xargs echo a b | tr ' ' '\\n' | grep -v '^[ab]$' | tr '\\n' ' '").strip() == " ".join(map(str, range(1, 101))))
    # and 3072 bytes, each word's NUL counted
    run("yes " + "x" * 59 + " | head -n 60 > xw")
    lines = rr("xargs echo < xw").split("\n")
    t.check("xargs cuts a run at 3072 bytes", len(lines) == 2 and len(lines[0].split()) == 51 and len(lines[1].split()) == 9, [len(l.split()) for l in lines])
    run("yes y | head -n 3066 | tr -d '\\n' > big1")
    run("yes y | head -n 3067 | tr -d '\\n' > big2")
    out, rc = st("xargs echo < big1 | wc -c")
    t.check("an item that fills the last byte goes through", out.strip() == "3067" and rc == 0, out)
    out, rc = st("xargs echo < big2")
    t.check("an item one byte too long is an error", out == "xargs: argument line too long" and rc == 1, out)
    out, rc = st("xargs -I{} echo {} < big2")
    t.check("so is one that -I makes too long", out == "xargs: argument line too long" and rc == 1, out)
    out, rc = st("xargs -I{} echo {} < big1 | wc -c")
    t.check("-I fills the last byte too", out.strip() == "3067" and rc == 0, out)
    out, rc = st("xargs -I{} echo {} '' < big1")
    t.check("-I counts the NUL of an empty word after a full one", out == "xargs: argument line too long" and rc == 1, out)
    # two items that between them fill the 3072 exactly, and two that are a byte over
    run("yes y | head -n 1533 | tr -d '\\n' > p1")
    run("yes y | head -n 1532 | tr -d '\\n' > p2")
    run("cat p1 > pp")
    run("echo -n ' ' >> pp")
    run("cat p2 >> pp")
    run("cat p1 > pq")
    run("echo -n ' ' >> pq")
    run("cat p1 >> pq")
    out, rc = st("xargs echo < pp | wc -l")
    t.check("two items that fill the 3072 bytes share a run", out.strip() == "1" and rc == 0, out)
    out, rc = st("xargs echo < pq | wc -l")
    t.check("two that are one byte over do not", out.strip() == "2" and rc == 0, out)
    run("rm xw big1 big2 p1 p2 pp pq")
    w58 = " ".join(str(i % 10) for i in range(58))
    lines = rr("seq 1 12 | xargs echo " + w58).split("\n")
    t.check("a long command leaves room for fewer items", len(lines) == 3 and [len(l.split()) for l in lines] == [63, 63, 60] and lines[2].endswith(" 12"), lines)
    # a command of 63 words is all xargs can be given to start with, and it runs without items
    out, rc = st("seq 1 62 | xargs xargs echo")
    t.check("a command of 63 words runs by itself with no input", out.split() == [str(i) for i in range(1, 63)] and rc == 0, out)
    lines = rr("seq 1 70 | xargs xargs echo").split("\n")
    t.check("and xargs runs xargs", len(lines) == 2 and lines[1].split() == [str(i) for i in range(63, 71)], lines)
    t.check("many runs in a row", run("seq 1 400 | xargs -n1 echo | wc -l").strip() == "400")
    out, rc = st("seq 1 400 | xargs -n1 true")
    t.check("many runs in a row with no output", out == "" and rc == 0, out)
    # exit statuses
    out, rc = st("echo a | xargs exitwith 3")
    t.check("a run that fails makes xargs exit 123", out == "" and rc == 123, out)
    out, rc = st("echo a b c | xargs -n1 exitwith 3")
    t.check("and the runs after it still happen", rc == 123, out)
    out, rc = st("echo a | xargs exitwith 0")
    t.check("a run that works leaves xargs at 0", out == "" and rc == 0, out)
    out, rc = st("echo a | xargs exitwith 126")
    t.check("a child's 126 is just a failure", out == "" and rc == 123, out)
    out, rc = st("echo a b | xargs -n1 exitwith 255")
    t.check("a run that exits 255 stops xargs with 124", out == "xargs: exitwith: exited with status 255; aborting" and rc == 124, out)
    out, rc = st("echo a b | xargs -n1 crash")
    t.check("a run that crashes stops xargs with 125", out.count("xargs: crash: terminated by a fault") == 1 and rc == 125, out)
    run("echo hi > nx")
    out, rc = st("echo a | xargs ./nx")
    t.check("a command that cannot start is 126", out.startswith("xargs: ./nx: ") and rc == 126, out)
    out, rc = st("echo a | xargs ./nosuch")
    t.check("a command that is not there is 127", out == "xargs: ./nosuch: no such file or directory" and rc == 127, out)
    out, rc = st("echo a b | xargs -n1 ./nosuch")
    t.check("and xargs stops there", out == "xargs: ./nosuch: no such file or directory" and rc == 127, out)
    out, rc = st("echo a | xargs cd")
    t.check("shell built-ins are not programs", out == "xargs: cd: no such file or directory" and rc == 127, out)
    run("rm nx")
    # the command line
    for b, msg in [("-z", ""), ("--nosuch", ""), ("-n 0", "value 0 for -n option should be >= 1"), ("-n x", 'invalid number "x" for -n option'),
                   ("--max-args=0", "value 0 for -n option should be >= 1"), ("--max-args=x", 'invalid number "x" for -n option'), ("--replace=", ""), ("-nx", 'invalid number "x" for -n option'),
                   ("-n -1", 'invalid number "-1" for -n option'), ("-0z", ""), ("-s 5", ""), ("-L 1", ""), ("-P 2", "")]:
        out, rc = st(f"echo x | xargs {b} echo")
        t.check(f"xargs {b} is refused", "usage: xargs" in out and out.startswith(("xargs: " + msg) if msg else "usage: xargs") and rc == 2, out)
    for b in ["-n", "-I", "--max-args", "-0n", "-tI"]:
        out, rc = st(f"echo x | xargs {b}")
        t.check(f"xargs {b} with nothing after it is refused", out == "usage: xargs [-0rt] [-n N] [-I STR] [COMMAND [ARGS...]]" and rc == 2, out)
    out, rc = st("echo x | xargs -n '' echo")
    t.check("an empty -n is refused", out.startswith('xargs: invalid number "" for -n option') and rc == 2, out)
    out, rc = st("echo x | xargs --max-argsx=3 echo")
    t.check("a long option that only starts like --max-args is refused", out == "usage: xargs [-0rt] [-n N] [-I STR] [COMMAND [ARGS...]]" and rc == 2, out)
    out, rc = st("echo x | xargs -")
    t.check("a lone - is the command, not an option", out == "xargs: -: no such file or directory" and rc == 127, out)
    out, rc = st("echo x | xargs -n 99999999999 echo")
    t.check("a huge -n just means as many as fit", out == "x" and rc == 0, out)
    out, rc = st("echo x | xargs -- -z")
    t.check("a command after -- may look like an option", out == "xargs: -z: no such file or directory" and rc == 127, out)
    out, rc = st("echo x | xargs echo -z")
    t.check("and so may its arguments, after the command's name", out == "-z x" and rc == 0, out)
    # -t goes to standard error, the rest to standard output
    out, rc = st("echo 'a b' | xargs -t echo > xo")
    t.check("-t writes the command line to the screen when the output is a file", out == "echo a b" and rc == 0, out)
    t.check("and the output went to the file", rr("cat xo") == "a b", rr("cat xo"))
    # every run writes after the one before in a file
    run("echo first > xo")
    run("echo 'a b c' | xargs -n1 echo >> xo")
    t.check("runs append to a redirected output", rr("cat xo") == "first\na\nb\nc", rr("cat xo"))
    run("echo 'a b c' | xargs -n1 echo > xo")
    t.check("runs after the first do not write over it", rr("cat xo") == "a\nb\nc", rr("cat xo"))
    run("rm xo")
    # standard input from the keyboard
    m.send("xargs echo hi\r")
    time.sleep(0.3)
    m.send("one two\r")
    time.sleep(0.2)
    m.send("three\r")
    time.sleep(0.2)
    m.send("\x04")
    raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
    t.check("xargs reads the keyboard until Ctrl+D", "hi one two three\n" in raw, repr(raw))
    # a last line without its Enter comes out at the first Ctrl+D; the second ends the input,
    # and xargs must not ask the keyboard again for the word it was in the middle of
    m.send("xargs echo hi\r")
    time.sleep(0.3)
    m.send("one two")
    time.sleep(0.2)
    m.send("\x04")
    time.sleep(0.3)
    m.send("\x04")
    raw = re.sub(r"\x1b\[[0-9;]*m", "", m.expect(PROMPT, 15)).replace("\r\n", "\n")
    t.check("xargs stops at the end of the input with a word still open", "hi one two\n" in raw, repr(raw))
    # find | xargs
    run("mkdir xd")
    run("echo A > xd/a.txt")
    run("echo B > xd/b.txt")
    run("echo C > xd/c.dat")
    t.check("find | xargs cat", rr("find xd -name '*.txt' | sort | xargs cat") == "A\nB", rr("find xd -name '*.txt' | sort | xargs cat"))
    t.check("find -print0 | xargs -0", rr("find xd -name 'c.*' -print0 | xargs -0 cat") == "C", rr("find xd -name 'c.*' -print0 | xargs -0 cat"))
    run("find xd -name '*.txt' | xargs rm")
    t.check("xargs rm", "c.dat" in rr("ls xd") and ".txt" not in rr("ls xd"), rr("ls xd"))
    run("rm -r xd")

    # ---- v0.32.0: du: the directories, the errors, and the cluster rounding the generated cases leave out ----
    def sized(path, n):
        run(f"yes | head -c {n} > {path}")
    def lines(c):
        return sorted(rr(c).split("\n"))
    run("mkdir dt")
    run("mkdir dt/s")
    run("mkdir dt/s/u")
    run("mkdir dt/e")
    sized("dt/a", 1536)
    sized("dt/s/b", 2048)
    sized("dt/s/u/c", 512)
    t.check("du lists each directory with all that is under it", lines("du dt") == ["0\tdt/e", "1\tdt/s/u", "3\tdt/s", "4\tdt"], lines("du dt"))
    t.check("du -a lists each file once", lines("du -a dt") == sorted(["0\tdt/e", "2\tdt/a", "1\tdt/s/u", "1\tdt/s/u/c", "2\tdt/s/b", "3\tdt/s", "4\tdt"]), lines("du -a dt"))
    t.check("du -s gives the one line", rr("du -s dt") == "4\tdt", rr("du -s dt"))
    t.check("du -sh", rr("du -sh dt") == "4.0K\tdt", rr("du -sh dt"))
    t.check("du -s with several paths keeps their order", rr("du -s dt/s dt/a dt/e") == "3\tdt/s\n2\tdt/a\n0\tdt/e", rr("du -s dt/s dt/a dt/e"))
    t.check("an empty directory is 0", rr("du dt/e") == "0\tdt/e", rr("du dt/e"))
    t.check("a trailing slash stays as typed", lines("du -a dt/s/") == sorted(["1\tdt/s/u", "1\tdt/s/u/c", "2\tdt/s/b", "3\tdt/s/"]), lines("du -a dt/s/"))
    t.check("du -c adds the grand total, rounded once at the end", rr("du -sc dt/s dt/a") == "3\tdt/s\n2\tdt/a\n4\ttotal", rr("du -sc dt/s dt/a"))
    run("cd dt/s")
    t.check("du with no path walks .", lines("du") == ["1\t./u", "3\t."], lines("du"))
    run("cd ..")
    run("cd ..")
    # a file takes whole clusters (512 bytes on the test disk)
    sized("dt/one", 1)
    sized("dt/c513", 513)
    sized("dt/c700", 700)
    t.check("1 byte takes a cluster", rr("du -h dt/one") == "512\tdt/one", rr("du -h dt/one"))
    t.check("513 bytes take two", rr("du -h dt/c513") == "1.0K\tdt/c513", rr("du -h dt/c513"))
    t.check("so do 700", rr("du -h dt/c700") == "1.0K\tdt/c700", rr("du -h dt/c700"))
    t.check("and a total counts clusters, not bytes", rr("du -s dt/one dt/one dt/one") == "1\tdt/one\n1\tdt/one\n1\tdt/one" and rr("du -sc dt/one dt/one dt/one").endswith("2\ttotal"), rr("du -sc dt/one dt/one dt/one"))
    out, rc = st("du nosuch dt/a")
    t.check("a missing path is reported and the rest still done", out == "du: nosuch: no such file or directory\n2\tdt/a" and rc == 1, out)
    out, rc = st("du -sa dt")
    t.check("-a with -s is refused", out.startswith("du: cannot both summarize and show all entries") and "usage: du" in out and rc == 2, out)
    out, rc = st("du -z dt")
    t.check("an unknown option is refused", out == "usage: du [-achks] [PATH...]" and rc == 2, out)
    out, rc = st("du -s dt | wc -l")
    t.check("du into a pipe", out.strip() == "1" and rc == 0, out)
    t.check("du -k is the default", rr("du -sk dt") == rr("du -s dt"))
    # what du says against what the disk says: the files' clusters, a little under what df lost
    def used_kb():
        mm = re.search(r"\s(\d+)\s+\d+\s+\d+%", rr("df"))
        return int(mm.group(1)) if mm else -1
    before = used_kb()
    run("mkdir dg")
    sized("dg/big", 200 * 1024)
    after = used_kb()
    t.check("du of a 200K file is 200 and df lost about as much", rr("du -s dg") == "200\tdg" and 200 <= after - before <= 204, (before, after))
    run("rm -r dg dt")

    # ---- v0.32.0: cal: today, the clock and the exit statuses the generated cases cannot show ----
    mm = re.match(r"(\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+)", rr("date"))
    clock_t0, clock_start = time.time(), (datetime.datetime(*(int(g) for g in mm.groups())) if mm else None)
    ty, tm, td = (int(g) for g in mm.groups()[:3]) if mm else (0, 0, 0)
    plain = rr("cal")
    mm2 = re.match(r"(\d+)-(\d+)-(\d+) ", rr("date"))
    if mm2 and (int(mm2.group(1)), int(mm2.group(2)), int(mm2.group(3))) == (ty, tm, td):  # not across midnight
        t.check("cal with no arguments is this month", plain == rr(f"cal {tm} {ty}"), plain)
        t.check("cal -y with no arguments is this year", rr("cal -y") == rr(f"cal {ty}"))
        t.check("cal -3 with no arguments is this month and its neighbours", rr("cal -3") == rr(f"cal -3 {tm} {ty}"))
        mark = f"\x1b[7m{td:2d}\x1b[27m"
        for c in ("cal", f"cal {tm} {ty}", "cal -y", f"cal {ty}", "cal -3", f"cal -3 {tm} {ty}", f"cal -1 {tm} {ty}"):
            raw = m.run(c)
            t.check(f"on the terminal, {c} shows today in reverse", raw.count("\x1b[7m") == 1 and mark in raw, raw)
        t.check("piped, nothing is highlighted", "\x1b[7m" not in m.run("cal | cat") and "\x1b[7m" not in m.run("cal -y | cat"))
        run("cal > cf")
        t.check("nor in a file", "\x1b[7m" not in m.run("cat cf") and run("cat cf").rstrip("\n") == plain)
        run("rm cf")
        t.check("another year has no today to show", "\x1b[7m" not in m.run(f"cal {tm} {ty - 1}") and "\x1b[7m" not in m.run(f"cal {ty + 1}"))
    else:
        t.check("cal: the clock gave a date", False, (plain, mm2))
    t.check("a month of another year, on the terminal, is plain", "\x1b[7m" not in m.run("cal 3 2000"))

    # with the clock set, so that the no-argument forms and the highlight have known answers: a one-digit day,
    # the last day of a year, the first, a leap day, and the middle months that make -3 a row of the year
    def at(y, mo, d):
        out = run(f"rtcset {y} {mo} {d} 12 0 0")
        t.check(f"the clock set to {y}-{mo}-{d}", "set=0" in out, out)

    def lit(c, day):  # on the terminal, c shows exactly one day in reverse, and it is this one
        raw = m.run(c)
        return raw.count("\x1b[7m") == 1 and f"\x1b[7m{day:2d}\x1b[27m" in raw
    at(2031, 5, 6)
    t.check("cal with the clock at 6 May 2031 is May 2031", rr("cal") == rr("cal 5 2031") and "May 2031" in rr("cal"), rr("cal"))
    t.check("and shows the 6th, with its space, in reverse", lit("cal", 6) and lit("cal 5 2031", 6) and lit("cal -1", 6), m.run("cal"))
    t.check("in a year", lit("cal -y", 6) and lit("cal 2031", 6) and rr("cal -y") == rr("cal 2031"))
    t.check("and with -3, where May is the middle of a row of the year", rr("cal -3") == rr("cal -3 5 2031") and "April" in rr("cal -3") and lit("cal -3", 6))
    t.check("not in April, nor May of another year", "\x1b[7m" not in m.run("cal 4 2031") and "\x1b[7m" not in m.run("cal 5 2030") and "\x1b[7m" not in m.run("cal 5 2032"))
    t.check("and -3 on the terminal for a month beside it shows it too", lit("cal -3 4 2031", 6) and lit("cal -3 6 2031", 6) and "\x1b[7m" not in m.run("cal -3 7 2031"))
    at(2031, 12, 31)
    t.check("on the last day of the year, -3 reaches into the next", rr("cal -3") == rr("cal -3 12 2031") and "January 2032" in rr("cal -3") and "November 2031" in rr("cal -3"))
    t.check("and shows the 31st once", lit("cal", 31) and lit("cal -3", 31) and lit("cal -y", 31))
    at(2031, 1, 1)
    t.check("on the first, it reaches back", rr("cal -3") == rr("cal -3 1 2031") and "December 2030" in rr("cal -3"))
    t.check("and shows the 1st once, in January", lit("cal", 1) and lit("cal -3", 1) and lit("cal -y", 1))
    at(2032, 2, 29)
    t.check("a leap day is this month's last", rr("cal") == rr("cal 2 2032") and lit("cal", 29) and lit("cal -3", 29) and lit("cal -y", 29))
    t.check("and -3 is a row of the year again", rr("cal -3") == rr("cal -3 2 2032") and rr("cal -3").startswith(" " * 28 + "2032"))
    at(2031, 7, 15)
    t.check("July is the start of a row of the year, so -3 has the neighbours", rr("cal -3") == rr("cal -3 7 2031") and "June 2031" in rr("cal -3") and "August 2031" in rr("cal -3"))
    # put the clock back where it was, plus the time that has passed
    if clock_start:
        n = clock_start + datetime.timedelta(seconds=int(time.time() - clock_t0) + 1)
        out = run(f"rtcset {n.year} {n.month} {n.day} {n.hour} {n.minute} {n.second}")
        t.check("the clock put back", "set=0" in out, out)

    def st(c):  # (output, exit status): the shell prints [exit status N] for a failure
        out = run(c).rstrip("\n")
        mm = re.search(r"\n?\[exit status (\d+)\]$", out)
        return (out[:mm.start()] if mm else out), (int(mm.group(1)) if mm else 0)

    usage = "usage: cal [-1 | -3 | -y] [[MONTH] YEAR]"
    for c in ("cal 1 2 3", "cal -q", "cal -j 2026", "cal -m 3", "cal -y 1 2 3", "cal 3 2026 2026", "cal -a", "cal -1y3q"):
        out, rc = st(c)
        t.check(f"{c}: the usage and status 2", out == usage and rc == 2, out)
    for c, msg in (("cal 13 2026", "cal: 13 is neither a month number (1..12) nor a name"),
                   ("cal 3 0", "cal: year `0' not in range 1..9999"),
                   ("cal 10000", "cal: year `10000' not in range 1..9999"),
                   ("cal abc", "cal: not a valid year abc"),
                   ("cal -3 2026", "cal: -3 together with a given year but no given month is not supported."),
                   ("cal -1 2026", "cal: -1 together with a given year but no given month is not supported."),
                   ("cal 2026 3", "cal: 2026 is neither a month number (1..12) nor a name"),
                   # numbers that would wrap around into range if they were allowed to (BSD cal lets 4294967297 be 1)
                   ("cal 3 4294967297", "cal: year `4294967297' not in range 1..9999"),
                   ("cal 4294967299 2026", "cal: 4294967299 is neither a month number (1..12) nor a name"),
                   ("cal 3 18446744073709551617", "cal: year `18446744073709551617' not in range 1..9999"),
                   ("cal 18446744073709551619 2026", "cal: 18446744073709551619 is neither a month number (1..12) nor a name"),
                   ("cal 4294967297", "cal: year `4294967297' not in range 1..9999")):
        out, rc = st(c)
        t.check(f"{c}: the message and status 2", out == msg and rc == 2, out)
    out, rc = st("cal 3 2026")
    t.check("a good month ends with status 0", rc == 0 and out.startswith("     March 2026"), (out, rc))
    out, rc = st("cal -y 2026 | wc -l")
    t.check("cal into a pipe", out.strip() == "36" and rc == 0, out)

    # ---- v0.33.0: base64: usage errors and statuses, which the generated cases (GNU's own wording and status 1) cannot show ----
    usage = "usage: base64 [-d] [-i] [-w COLS] [FILE]"
    for c, msg in (("base64 -x", "base64: invalid option -- 'x'"),
                   ("base64 -dx", "base64: invalid option -- 'x'"),
                   ("base64 --foo", "base64: unrecognized option '--foo'"),
                   ("base64 --decodex", "base64: unrecognized option '--decodex'"),
                   ("base64 --", None),
                   ("base64 a b", "base64: extra operand 'b'"),
                   ("base64 - -", "base64: extra operand '-'"),
                   ("base64 -w", "base64: option requires an argument -- 'w'"),
                   ("base64 -dw", "base64: option requires an argument -- 'w'"),
                   ("base64 --wrap", "base64: option '--wrap' requires an argument"),
                   ("base64 --decode=1", "base64: option '--decode=1' doesn't allow an argument"),
                   ("base64 --ignore-garbage=1", "base64: option '--ignore-garbage=1' doesn't allow an argument")):
        if msg is None:
            continue
        out, rc = st(c)
        t.check(f"{c}: the message, the usage and status 2", out == msg + "\n" + usage and rc == 2, out)
    for w in ("5x", "-1", "-5", "+", "-", "0x10", "3 ", "1e3", "abc", "--5", "+-5", "''", "' '"):
        arg = w if w.startswith("'") else f"'{w}'"
        shown = w.strip("'")
        out, rc = st(f"echo hello | base64 -w {arg}")
        t.check(f"-w {w} is an invalid wrap size, status 2", out == f"base64: invalid wrap size: '{shown}'" and rc == 2, out)
    out, rc = st("echo hello | base64 --wrap=5x")
    t.check("--wrap=5x too", out == "base64: invalid wrap size: '5x'" and rc == 2, out)
    out, rc = st("echo hello | base64")
    t.check("a good encoding ends with status 0", out == "aGVsbG8K" and rc == 0, (out, rc))
    out, rc = st("echo aGVsbG8K | base64 -d")
    t.check("and a good decoding", out == "hello" and rc == 0, (out, rc))
    out, rc = st("echo aGVsbG8 | base64 -d")
    t.check("an invalid input is status 1, after what came before it", out == "hellobase64: invalid input" and rc == 1, (out, rc))
    out, rc = st("base64 nosuchfile")
    t.check("a file that is not there is status 1", out == "base64: nosuchfile: no such file or directory" and rc == 1, (out, rc))
    out, rc = st("yes | base64 | head -n 2")
    t.check("a reader that goes away ends it (an endless input, two lines)", out == "eQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5\nCnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkKeQp5CnkK" and rc == 0, (out, rc))


def grepwc_tests(m, t):
    """v0.24.8: grep on real regular expressions (the engine itself is
    checked against GNU grep by tests/test_regex.c) with its output and
    exit-status options, and wc with flags."""
    def run(c, timeout=30):
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    def st(c):  # (output, exit status): the shell prints [exit status N] for a failure
        out = run(c)
        mm = re.search(r"\n?\[exit status (\d+)\]$", out)
        return (out[:mm.start()] if mm else out), (int(mm.group(1)) if mm else 0)

    def words(c):
        return re.sub(r"\s+", " ", run(c)).strip()

    run("echo -e 'one two\\nthree\\n\\nfour five six' > gw.txt")   # 4 lines, 6 words, 29 bytes

    # ---- wc ----
    t.check("wc with no flags is lines, words, bytes, 7 wide", run("wc gw.txt") == "      4       6      29 gw.txt", run("wc gw.txt"))
    t.check("wc -l names the file", run("wc -l gw.txt") == "4 gw.txt")
    t.check("a single number from standard input is bare", run("wc -l < gw.txt") == "4" and run("cat gw.txt | wc -w") == "6" and run("cat gw.txt | wc -c") == "29")
    t.check("wc -m counts the same bytes", run("wc -m < gw.txt") == "29")
    t.check("the columns come in a fixed order whatever the flags", words("wc -cl gw.txt") == "4 29 gw.txt" and words("wc -wl gw.txt") == "4 6 gw.txt")
    t.check("flags may be bundled or apart", words("wc -lwc gw.txt") == words("wc -l -w -c gw.txt") == "4 6 29 gw.txt")
    t.check("wc -L is the longest line", words("wc -L gw.txt") == "13 gw.txt")
    t.check("and a tab runs to the next multiple of 8", run("echo -e 'ab\\tc' | wc -L") == "9" and run("echo -e '\\tc' | wc -L") == "9")
    t.check("a carriage return starts the line over", run("echo -e 'abcdef\\rxy' | wc -L") == "6")
    t.check("all four", words("wc -lwcL gw.txt") == "4 6 29 13 gw.txt")
    t.check("\\v, \\f and \\r separate words", run("echo -e 'a\\013b\\014c\\rd' | wc -w") == "4")
    t.check("empty input is all zeros", words("echo -n | wc") == "0 0 0")
    t.check("a last line without a newline is a word but not a line", words("echo -n 'a b' | wc") == "0 2 3")
    run("echo -e 'x\\ny' > gw2.txt")
    out = run("wc -l gw.txt gw2.txt")
    t.check("several files get a total, 7 wide", [re.sub(r"\s+", " ", l).strip() for l in out.split("\n")] == ["4 gw.txt", "2 gw2.txt", "6 total"] and out.split("\n")[0].startswith("      4"), out)
    out = run("wc -L gw.txt gw2.txt")
    t.check("the total of -L is the longest of all", words("wc -L gw.txt gw2.txt").endswith("13 total"), out)
    t.check("- is standard input", run("echo hi | wc -c -") == "3 -" and words("echo hi | wc - gw2.txt").startswith("1 1 3 -"))
    out, rc = st("wc -l nosuch gw2.txt")
    t.check("a missing file is reported and the rest counted", "nosuch: no such file" in out and "2 gw2.txt" in out and rc == 1, out)
    out, rc = st("wc -Z gw.txt")
    t.check("a bad wc option shows the usage", "usage: wc [-lwcmL]" in out and rc == 2, out)
    t.check("wc -- -x? takes -- as the end of options", run("wc -l -- gw2.txt") == "2 gw2.txt")
    out, rc = st("wc gw2.txt dd")
    t.check("a directory is an error, not a count", rc == 1 and "dd" in out, out)

    # ---- grep: syntax ----
    run("echo -e 'abc\\nABC\\na.c\\nxabcx\\nabcabc\\nfoo bar\\n(paren)\\na|b\\naaa' > gr.txt")
    def g(c):  # the output of a grep as a list of lines
        o = st(c)[0]
        return o.split("\n") if o else []
    t.check("plain text still works", g("grep abc gr.txt") == ["abc", "xabcx", "abcabc"])
    t.check(". matches any character", g("grep 'a.c' gr.txt") == ["abc", "a.c", "xabcx", "abcabc"])
    t.check("\\. and [.] are a dot", g("grep 'a\\.c' gr.txt") == ["a.c"] and g("grep 'a[.]c' gr.txt") == ["a.c"])
    t.check("anchors", g("grep '^abc' gr.txt") == ["abc", "abcabc"] and g("grep 'abc$' gr.txt") == ["abc", "abcabc"] and g("grep '^abc$' gr.txt") == ["abc"])
    t.check("a bracket expression and a range", g("grep '^[a-c]*$' gr.txt") == ["abc", "abcabc", "aaa"] and g("grep -c '[[:upper:]]' gr.txt") == ["1"])
    t.check("basic syntax: ( | + ? are plain", g("grep '(paren)' gr.txt") == ["(paren)"] and g("grep 'a|b' gr.txt") == ["a|b"])
    t.check("basic syntax: \\| \\( \\) \\+ \\?", g("grep -c 'foo\\|bar' gr.txt") == ["1"] and g("grep '^\\(abc\\)\\+$' gr.txt") == ["abc", "abcabc"] and g("grep -c '^ab\\?c$' gr.txt") == ["1"])
    t.check("intervals", g("grep 'a\\{3\\}' gr.txt") == ["aaa"] and g("grep -E '^a{1,2}b' gr.txt") == ["abc", "abcabc"])
    t.check("-E: | ( ) + ?", g("grep -E 'foo|^a.c$' gr.txt") == ["abc", "a.c", "foo bar"] and g("grep -E '^(abc)+$' gr.txt") == ["abc", "abcabc"] and g("grep -Ec '^ab?c$' gr.txt") == ["1"])
    t.check("-E: \\( is a plain (", g("grep -E '\\(paren\\)' gr.txt") == ["(paren)"])
    t.check("-F takes it all literally", g("grep -F 'a.c' gr.txt") == ["a.c"] and g("grep -F '(paren)' gr.txt") == ["(paren)"] and g("grep -F 'a|b' gr.txt") == ["a|b"])
    t.check("-G is the default again", g("grep -E -G 'a|b' gr.txt") == ["a|b"])
    t.check("\\w \\W \\s \\b \\< \\>", g("grep -c '\\w\\s\\w' gr.txt") == ["1"] and g("grep 'foo\\>' gr.txt") == ["foo bar"] and g("grep '\\<bar' gr.txt") == ["foo bar"] and g("grep -c 'o\\Bo' gr.txt") == ["1"])
    t.check("* at the start is a plain *", g("grep -c '*' gr.txt") == ["0"] and run("echo 'a*b' | grep '*b'") == "a*b")
    t.check("longest match wins with alternatives", run("echo abcd | grep -oE 'ab|abcd|abc'") == "abcd")
    t.check("a back-reference is refused and says so", "back-references are not supported" in run("grep '\\(a\\)\\1' gr.txt"))
    out, rc = st("grep '\\(' gr.txt")
    t.check("a bad pattern is exit 2 with GNU's message", out == "grep: Unmatched ( or \\(" and rc == 2, out)
    out, rc = st("grep '[a' gr.txt")
    t.check("an open bracket is too", "grep: Unmatched [" in out and rc == 2, out)
    out, rc = st("grep -E 'a{2,1}' gr.txt")
    t.check("and a backwards interval", "Invalid content" in out and rc == 2, out)

    # ---- grep: selecting ----
    t.check("-i", g("grep -i abc gr.txt") == ["abc", "ABC", "xabcx", "abcabc"] and g("grep -y ABC gr.txt")[0] == "abc")
    t.check("-i reaches ranges and classes", g("grep -ic '^[a-c]*$' gr.txt") == ["4"] and g("grep -ic '[[:lower:]]' gr.txt") == ["9"])
    t.check("-v lists the others", g("grep -v -e abc -e 'a.' gr.txt") == ["ABC"] and g("grep -vc abc gr.txt") == ["6"])
    t.check("-w", g("grep -w abc gr.txt") == ["abc"] and g("grep -w a gr.txt") == ["a.c", "a|b"])
    t.check("-w finds a later match when the first is inside a word", run("echo 'xfoo foo' | grep -ow foo") == "foo" and st("echo 'foox' | grep -w foo") == ("", 1))
    t.check("-x", g("grep -x abc gr.txt") == ["abc"] and g("grep -xE 'a.c|foo' gr.txt") == ["abc", "a.c"] and g("grep -xF a.c gr.txt") == ["a.c"])
    t.check("-e twice", g("grep -e foo -e paren gr.txt") == ["foo bar", "(paren)"])
    t.check("-e takes a pattern that looks like an option", run("echo 'a -x b' | grep -e -x") == "a -x b" and run("echo 'a -x b' | grep -- -x") == "a -x b")
    run("echo -e 'foo\\n(p' > gpat.txt")
    t.check("-f reads patterns", g("grep -F -f gpat.txt gr.txt") == ["foo bar", "(paren)"])
    run("echo -n > gempty.txt")
    out, rc = st("grep -f gempty.txt gr.txt")
    t.check("-f of an empty file matches nothing", out == "" and rc == 1, out)
    t.check("and with -v, everything", g("grep -vc -f gempty.txt gr.txt") == ["9"])
    t.check("an empty pattern matches every line", g("grep -c '' gr.txt") == ["9"] and g("grep -c -e '' -e zzz gr.txt") == ["9"])
    t.check("-f combined with -e", g("grep -c -F -f gpat.txt -e abc gr.txt") == ["5"])
    t.check("-m stops early", g("grep -m 2 abc gr.txt") == ["abc", "xabcx"] and g("grep -m1 -c abc gr.txt") == ["1"])
    out, rc = st("grep -m 0 abc gr.txt")
    t.check("-m 0 selects nothing", out == "" and rc == 1, out)

    # ---- grep: output ----
    t.check("-n", g("grep -n abc gr.txt") == ["1:abc", "4:xabcx", "5:abcabc"])
    t.check("-c", g("grep -c abc gr.txt") == ["3"] and g("grep -c zzz gr.txt") == ["0"])
    t.check("-o prints each match on a line", g("grep -o abc gr.txt") == ["abc"] * 4)
    out = g("grep -on abc gr.txt")
    t.check("-o with -n prefixes every match", out == ["1:abc", "4:abc", "5:abc", "5:abc"], str(out))
    t.check("-o skips empty matches", run("echo abc | grep -o 'x*'") == "")
    t.check("-o with -i keeps the original case", run("echo 'AbC abc' | grep -oi abc") == "AbC\nabc")
    t.check("-ov prints nothing", run("grep -ov abc gr.txt") == "")
    t.check("-l and -L", g("grep -l abc gr.txt gw.txt") == ["gr.txt"] and g("grep -L abc gr.txt gw.txt") == ["gw.txt"])
    out, rc = st("grep -L abc gr.txt")
    t.check("-L exits 1 when every file matched", out == "" and rc == 1, out)
    out, rc = st("grep -L zzz gr.txt")
    t.check("and 0 when one was listed", out == "gr.txt" and rc == 0, out)
    t.check("-l stops at the first match", g("grep -l -c abc gr.txt") == ["gr.txt"])
    t.check("-q is silent", run("grep -q abc gr.txt") == "")
    out, rc = st("grep -q zzz gr.txt")
    t.check("and -q exit status is 1 for no match", out == "" and rc == 1, out)
    out, rc = st("grep -q abc nosuch gr.txt")
    t.check("-q with a match is 0 even after an error", rc == 0, out)
    t.check("two files get names", g("grep -n one gr.txt gw.txt") == ["gw.txt:1:one two"] and g("grep -c abc gr.txt gw.txt") == ["gr.txt:3", "gw.txt:0"])
    t.check("-h hides them, -H shows them", g("grep -h abc gr.txt gw.txt")[0] == "abc" and g("grep -H one gw.txt") == ["gw.txt:one two"])
    t.check("standard input is (standard input)", run("echo hi | grep -H hi") == "(standard input):hi" and run("echo hi | grep -c hi - gw.txt") == "(standard input):1\ngw.txt:0")
    t.check("- reads standard input", run("echo hello | grep -n ell -") == "1:hello")
    t.check("no newline at the end of the file is added", run("echo -n 'a b' | grep b | wc -c") == "4")

    # ---- grep: context ----
    run("echo -e 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj' > gc.txt")
    t.check("-A", g("grep -A1 -e b gc.txt") == ["b", "c"] and g("grep -A 2 -e b gc.txt") == ["b", "c", "d"])
    t.check("-B", g("grep -B1 -e c gc.txt") == ["b", "c"])
    t.check("-C and -N", g("grep -C1 e gc.txt") == ["d", "e", "f"] and g("grep -1 e gc.txt") == ["d", "e", "f"])
    t.check("-n marks matches with : and context with -", g("grep -n -C1 e gc.txt") == ["4-d", "5:e", "6-f"])
    t.check("groups that are apart get --", g("grep -C1 -e b -e i gc.txt") == ["a", "b", "c", "--", "h", "i", "j"])
    t.check("adjacent and overlapping groups are one", g("grep -C1 -e c -e e gc.txt") == ["b", "c", "d", "e", "f"] and g("grep -A1 -e b -e d gc.txt") == ["b", "c", "d", "e"])
    t.check("a match inside the context is a match", g("grep -n -A2 -e b -e c gc.txt") == ["2:b", "3:c", "4-d", "5-e"])
    t.check("context at the edges", g("grep -B3 a gc.txt") == ["a"] and g("grep -A3 j gc.txt") == ["j"])
    t.check("file names in context lines", g("grep -H -C1 e gc.txt") == ["gc.txt-d", "gc.txt:e", "gc.txt-f"])
    t.check("-- between files too", g("grep -A1 -e a -e one gc.txt gw.txt") == ["gc.txt:a", "gc.txt-b", "--", "gw.txt:one two", "gw.txt-three"])
    t.check("-m with trailing context", g("grep -m1 -A1 -e a -e b gc.txt") == ["a", "b"])
    t.check("-v with context", g("grep -v -A1 -e '[a-g]' gc.txt") == ["h", "i", "j"] and g("grep -vc -e '[a-g]' gc.txt") == ["3"])
    t.check("-c ignores the context options", g("grep -c -C2 e gc.txt") == ["1"])
    out, rc = st("grep -A x e gc.txt")
    t.check("a bad context length", "invalid context length" in out and rc == 2, out)
    out, rc = st("grep -m x e gc.txt")
    t.check("a bad -m", "invalid max count" in out and rc == 2, out)
    t.check("a large -B does not matter", g("grep -B 5000 c gc.txt") == ["a", "b", "c"])

    # ---- grep: lines of any length, binary files ----
    run("echo -n xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx > gl1")   # 80
    run("cat gl1 gl1 gl1 gl1 gl1 gl1 gl1 gl1 gl1 gl1 > gl2")   # 800
    run("cat gl2 gl2 gl2 gl2 gl2 gl2 gl2 gl2 gl2 gl2 > gl3")   # 8000
    run("echo needle >> gl3")
    run("echo tail >> gl3")
    t.check("a line of 8000 bytes", run("grep -c needle gl3") == "1" and run("grep needle gl3 | wc -c") == "8007" and run("grep -o needle gl3") == "needle")
    t.check("and one found by an anchored pattern at its far end", run("grep -c 'x*needle$' gl3") == "1" and run("grep -c '^x*needle$' gl3") == "1")
    t.check("the line after it is intact", run("grep tail gl3") == "tail" and run("grep -n tail gl3") == "2:tail")
    run("echo -e 'ab\\x00cd match\\nplain match' > gb.dat")
    t.check("binary file: just says so", run("grep match gb.dat") == "Binary file gb.dat matches")
    t.check("-a prints it anyway", run("grep -a plain gb.dat") == "plain match")
    t.check("-c and -l do not mind", run("grep -c match gb.dat") == "2" and run("grep -l match gb.dat") == "gb.dat")
    out, rc = st("grep zzz gb.dat")
    t.check("and exits 1", out == "" and rc == 1, out)

    # ---- grep: files, directories ----
    for c in ["mkdir gt", "mkdir gt/sub", "echo needle one > gt/a.txt", "echo nothing > gt/b.txt", "echo needle two > gt/sub/c.txt", "echo Needle three > gt/sub/D.txt"]:
        run(c)
    out, rc = st("grep needle gt")
    t.check("a directory without -r is an error", "gt: is a directory" in out and rc == 2, out)
    t.check("-s keeps quiet about it", st("grep -s needle gt") == ("", 2))
    t.check("-r walks it, sorted, with names", g("grep -r needle gt") == ["gt/a.txt:needle one", "gt/sub/c.txt:needle two"])
    t.check("-r -i", g("grep -ri needle gt") == ["gt/a.txt:needle one", "gt/sub/c.txt:needle two", "gt/sub/D.txt:Needle three"])
    t.check("-rl", g("grep -rl needle gt") == ["gt/a.txt", "gt/sub/c.txt"])
    t.check("-rL", g("grep -rL needle gt") == ["gt/b.txt", "gt/sub/D.txt"])
    t.check("-rc", g("grep -rc needle gt") == ["gt/a.txt:1", "gt/b.txt:0", "gt/sub/c.txt:1", "gt/sub/D.txt:0"])
    t.check("-rh", g("grep -rh needle gt") == ["needle one", "needle two"])
    t.check("-R is the same", g("grep -Rl needle gt") == ["gt/a.txt", "gt/sub/c.txt"])
    t.check("-r on a plain file has no name", g("grep -r needle gt/a.txt") == ["needle one"])
    t.check("-r on two paths", g("grep -rn nothing gt/b.txt gt/sub") == ["gt/b.txt:1:nothing"])
    t.check("a trailing slash does not double", g("grep -rl needle gt/") == ["gt/a.txt", "gt/sub/c.txt"])
    run("cd gt")
    t.check("-r with no file is ., with bare names", g("grep -r needle") == ["a.txt:needle one", "sub/c.txt:needle two"])
    t.check("-r . keeps the ./", g("grep -rl needle .") == ["./a.txt", "./sub/c.txt"])
    run("cd ..")
    t.check("a missing file does not stop the others, but exits 2", st("grep -c abc nosuch gr.txt")[1] == 2 and "gr.txt:3" in run("grep -c abc nosuch gr.txt"))
    out, rc = st("grep abc nosuch gr.txt")
    t.check("and the message names it", "grep: nosuch: no such file" in out and "gr.txt:abc" in out and rc == 2, out)
    t.check("-s hides that message, not the exit status", "nosuch" not in run("grep -s abc nosuch gr.txt") and st("grep -s abc nosuch gr.txt")[1] == 2)
    t.check("grep reading a pipe", run("echo 'x y' | grep -o 'y'") == "y" and run("cat gr.txt | grep -c abc") == "3")
    t.check("grep -m 1 ends the pipeline's reader early", run("cat gw.txt | grep -m 1 t") == "one two")
    out, rc = st("grep --help")
    t.check("an unknown long option shows the usage", "usage: grep" in out and rc == 2, out)
    for f in ["gw.txt", "gw2.txt", "gr.txt", "gc.txt", "gpat.txt", "gempty.txt", "gl1", "gl2", "gl3", "gb.dat"]:
        run("rm " + f)
    run("rm -r gt")

    # ---- ls sorts by name ----
    run("mkdir lsd")
    for c in ["touch lsd/zeta", "touch lsd/Alpha", "touch lsd/beta", "mkdir lsd/Mid", "touch lsd/Mid/y", "touch lsd/Mid/X", "mkdir lsd/aaa", "touch lsd/aaa/b", "touch lsd/aaa/a"]:
        run(c)
    def names(c):
        return [m_.group(1).rstrip("/") for l in run(c).split("\n") for m_ in [re.match(r"^\s+(\S+)/?\s+(?:<dir>|\d+)\s", l)] if m_]
    t.check("ls lists in name order, case ignored", names("ls lsd") == ["aaa", "Alpha", "beta", "Mid", "zeta"], str(names("ls lsd")))
    out = run("ls -R lsd")
    t.check("-R goes down in that order too", [l for l in out.split("\n") if l.endswith(":")] == ["lsd:", "lsd/aaa:", "lsd/Mid:"], out)
    t.check("and each directory is sorted", names("ls lsd/Mid") == ["X", "y"] and names("ls lsd/aaa") == ["a", "b"])
    for i in range(39, -1, -1):
        run("touch lsd/f%02d" % i)
    got = names("ls lsd")
    t.check("a directory of 45 entries, made in reverse, comes out sorted", got == sorted(got, key=str.lower) and len(got) == 45, str(got))
    run("rm -r lsd")


def http11_tests(m, t, base, fetch="fetch"):
    """The shapes of an HTTP/1.1 response, over http or https alike.
    Expects big.bin in the guest, to compare with."""
    out = plain(m.run(fetch + " -o ck.bin %s/chunked" % base, timeout=60))
    t.check("a body in chunks of every size", "saved 300000 bytes to ck.bin" in out, out)
    out = plain(m.run("cmp ck.bin big.bin", timeout=30))
    t.check("comes out the same as when sent whole", out.strip() == "", out)
    m.run("rm ck.bin")
    out = plain(m.run(fetch + " %s/chunked-slowly" % base, timeout=20))
    t.check("chunks that arrive a byte at a time", out.endswith("one piece at a time") and "exit status" not in out, out)
    out = plain(m.run(fetch + " -o cut.txt %s/chunked-cut" % base, timeout=20))
    t.check("a chunked body that stops early is an error", "closed before the end of the body" in out
            and "exit status 1" in out, out)
    t.check("and leaves no file", "no such file" in plain(m.run("cat cut.txt")))
    out = plain(m.run(fetch + " %s/chunked-bad" % base, timeout=20))
    t.check("a chunk length that is not a number", "hello" in out and "world" not in out and "malformed" in out
            and "exit status 1" in out, out)
    out = plain(m.run(fetch + " %s/chunked-huge" % base, timeout=20))
    t.check("or does not fit in 32 bits", "malformed" in out and "exit status 1" in out, out)
    out = plain(m.run(fetch + " %s/gzip" % base, timeout=20))
    t.check("an encoding fetch does not know is turned down", 'transfer encoding "gzip, chunked" is not supported' in out, out)
    out = plain(m.run(fetch + " -i %s/hints" % base, timeout=20))
    t.check("interim responses are passed over (and shown by -i)", "103 Early Hints" in out and "100 Continue" in out
            and out.endswith("after a hint"), out)
    out = plain(m.run(fetch + " %s/hints" % base, timeout=20))
    t.check("and are not part of the page", out.endswith("after a hint") and "Early" not in out, out)
    out = plain(m.run(fetch + " -i %s/nothing" % base, timeout=20))
    t.check("a response with no body", "204 No Content" in out and "exit status" not in out, out)
    out = plain(m.run(fetch + " %s/unsized" % base, timeout=20))
    t.check("a body with no stated length runs until the server closes", out.endswith("as long as it lasts")
            and "exit status" not in out, out)


def _one_shot(answer):
    """A server on a host port that takes whatever each client sends
    first and replies with answer(connection). Returns (socket, port)."""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(5)

    def loop():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            with c:
                c.settimeout(10)
                try:
                    answer(c)
                except OSError:
                    pass

    threading.Thread(target=loop, daemon=True).start()
    return srv, srv.getsockname()[1]


def _test_pki(work):
    """A small PKI for the TLS tests, valid around now: a root (P-256), an
    intermediate under it (RSA 2048), and server certificates for
    10.0.2.2: under the intermediate a P-384 key (so the server signs
    with ECDSA P-384), another for the wrong name, another out of its
    dates; under the root, an RSA key (so the server signs with RSA-PSS).
    Returns the root's PEM, an SSL context for each server, and each
    server's certificate chain and key as PEM; or None without Python's
    cryptography package."""
    try:
        from cryptography import x509 as cx509
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        import gen_x509_ours as g
    except ImportError:
        return None
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    span = lambda a, b: g.seq(g.utc((now + datetime.timedelta(days=a)).strftime("%y%m%d%H%M%SZ")),
                              g.utc((now + datetime.timedelta(days=b)).strftime("%y%m%d%H%M%SZ")))
    root = g.Cert(g.name(("CN", "RingTail Test Root")), g.ec_key(0x7E57_0001), exts=g.ca_exts())
    root.f["validity"] = span(-1, 3)
    inter = g.Cert(g.name(("CN", "RingTail Test Intermediate")), g.rsa_key(2048, 0x7E57), root, exts=g.ca_exts(0))
    inter.f["validity"] = span(-1, 3)

    def server(key, issuer, names, days=(-1, 2), h="sha256"):
        c = g.Cert(g.name(("CN", "ringtail test server")), key, issuer,
                   exts=[g.san(*names), g.ku("digitalSignature"), g.eku(g.SERVER_AUTH)], h=h)
        c.f["validity"] = span(*days)
        chain = [c] + ([issuer] if issuer is not root else [])
        cert_pem = "".join(cx509.load_der_x509_certificate(x.der()).public_bytes(serialization.Encoding.PEM).decode()
                           for x in chain)
        key_pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                    serialization.NoEncryption()).decode()
        pems.append((cert_pem, key_pem))
        cf, kf = os.path.join(work, "c.pem"), os.path.join(work, "k.pem")
        open(cf, "w").write(cert_pem)
        open(kf, "w").write(key_pem)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(cf, kf)
        return ctx

    here = [g.ip(10, 0, 2, 2), g.dns("ringtail.test")]
    pems = []
    p384 = g.ec_key(0x7E57_0002, ec.SECP384R1)
    ctxs = {"main": server(p384, inter, here, h="sha384"),
            "rsa": server(g.rsa_key(2048, 0x7E58), root, here),
            "name": server(g.ec_key(0x7E57_0003), inter, [g.dns("elsewhere.test")]),
            "expired": server(g.ec_key(0x7E57_0004), inter, here, days=(-10, -5))}
    old = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    old.maximum_version = ssl.TLSVersion.TLSv1_2
    old.load_cert_chain(os.path.join(work, "c.pem"), os.path.join(work, "k.pem"))
    ctxs["tls12"] = old
    root_pem = cx509.load_der_x509_certificate(root.der()).public_bytes(serialization.Encoding.PEM)
    return root_pem, ctxs, dict(zip(["main", "rsa", "name", "expired"], pems))


def _aes_only_server(pem, work):
    """openssl s_server, as the server for one connection, with
    TLS_AES_128_GCM_SHA256 its only suite, serving BIG as /big.bin (Python's
    ssl cannot narrow TLS 1.3's suites). Returns (process, port), or None
    when there is no openssl or it will not start."""
    if not shutil.which("openssl"):
        return None
    leaf, rest = pem[0].split("-----END CERTIFICATE-----\n", 1)
    open(os.path.join(work, "c.pem"), "w").write(leaf + "-----END CERTIFICATE-----\n")
    open(os.path.join(work, "i.pem"), "w").write(rest)  # -cert sends only the leaf
    open(os.path.join(work, "k.pem"), "w").write(pem[1])
    open(os.path.join(work, "big.bin"), "wb").write(BIG)
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    proc = subprocess.Popen(["openssl", "s_server", "-accept", "127.0.0.1:%d" % port, "-cert", "c.pem", "-cert_chain", "i.pem",
                             "-key", "k.pem", "-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256", "-naccept", "1", "-WWW"],
                            cwd=work, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ready = threading.Event()

    def watch():
        for line in proc.stdout:
            if line.startswith(b"ACCEPT"):
                ready.set()
    threading.Thread(target=watch, daemon=True).start()
    if not ready.wait(10):
        proc.kill()
        return None
    return proc, port


def tls_tests(m, t, web):
    """fetch over TLS 1.3, against OpenSSL (through Python's ssl) on the
    host: the certificate checked, against a test CA the guest is handed
    (--ca) or the roots on its disk; the same pages as over plain HTTP,
    redirects from one to the other, and servers that are not what fetch
    hoped for."""
    work = tempfile.mkdtemp(prefix="ringtail-tls-")
    try:
        pki = _test_pki(work)
    finally:
        shutil.rmtree(work)
    if not pki:
        print("  (Python's cryptography package is not installed, skipping the TLS tests)")
        return
    root_pem, ctxs, pems = pki
    ctx = ctxs["main"]
    _Http.extra["/ca.pem"] = root_pem

    sweb = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Http)
    sweb.agents = []
    sweb.socket = ctx.wrap_socket(sweb.socket, server_side=True, do_handshake_on_connect=False)
    sweb.handle_error = lambda request, address: None  # a client that refuses the certificate, as some must
    sweb.other, web.other = web.server_port, sweb.server_port
    threading.Thread(target=sweb.serve_forever, daemon=True).start()
    url, base = "10.0.2.2:%d" % web.server_port, "https://10.0.2.2:%d" % sweb.server_port
    warning = "the certificate of 10.0.2.2 was NOT checked"
    fetch = "fetch --ca ca.pem"

    def tls12(c):
        ctxs["tls12"].wrap_socket(c, server_side=True).close()

    def not_tls(c):
        c.recv(4096)
        c.sendall(b"HTTP/1.0 400 Bad Request\r\n\r\nthis port speaks plain HTTP\n")

    def garbage(c):
        c.recv(4096)
        c.sendall(bytes([22, 3, 3, 0, 60]) + bytes(60))  # a handshake record of zeros

    def page(which):
        def answer(c):
            s = ctxs[which].wrap_socket(c, server_side=True)
            s.recv(4096)
            s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\nserved by %-5s\n"
                      % which.encode())
            s.unwrap()
        return answer

    others = [_one_shot(f) for f in (tls12, not_tls, garbage, lambda c: c.recv(4096))]
    rsa, wrong_name, expired = [_one_shot(page(w)) for w in ("rsa", "name", "expired")]
    try:
        out = plain(m.run("fetch -o ca.pem http://%s/ca.pem" % url, timeout=20))
        t.check("the test CA, fetched into the guest", "saved %d bytes" % len(root_pem) in out, out)

        out = plain(m.run("fetch %s/hello.txt" % base, timeout=30))
        t.check("by default the roots on the disk are trusted, and the test CA is not one of them",
                "is not signed by anyone we trust" in out and "-k connects without checking" in out
                and "exit status 1" in out and "hello" not in out and warning not in out, out)
        out = plain(m.run("%s %s/hello.txt" % (fetch, base), timeout=20))
        t.check("with --ca, fetch gets a page over TLS 1.3, its certificate checked",
                out.endswith("hello from the host") and "exit status" not in out and warning not in out, out)
        t.check("the server got the request", sweb.agents and sweb.agents[-1].startswith("fetch (RingTail OS"), str(sweb.agents))
        out = plain(m.run("fetch -k %s/hello.txt" % base, timeout=20))
        t.check("-k gets it unchecked, and says so", out.endswith("hello from the host") and warning in out
                and "nothing proves who is at the other end" in out, out)
        out = plain(m.run("fetch -k %s/hello.txt > page.txt" % base, timeout=20))
        t.check("the warning is not part of the page", warning in out and plain(m.run("cat page.txt")).strip() == "hello from the host", out)
        m.run("rm page.txt")
        out = plain(m.run("%s https://10.0.2.2:%d/" % (fetch, rsa[1]), timeout=30))
        t.check("an RSA certificate, and the server's RSA-PSS signature", "served by rsa" in out and "exit status" not in out, out)
        out = plain(m.run("%s https://10.0.2.2:%d/" % (fetch, wrong_name[1]), timeout=20))
        t.check("a certificate for some other name is refused", "is for some other name" in out and "exit status 1" in out
                and "served" not in out, out)
        out = plain(m.run("%s https://10.0.2.2:%d/" % (fetch, expired[1]), timeout=20))
        t.check("so is one out of its dates", "has expired or is not yet valid" in out and "exit status 1" in out
                and "served" not in out, out)
        out = plain(m.run("fetch --ca nope.pem %s/hello.txt" % base, timeout=20))
        t.check("a --ca file that is not there", "nope.pem: " in out and "exit status 1" in out, out)
        out = plain(m.run("fetch --ca welcome.txt %s/hello.txt" % base, timeout=20))
        t.check("and one with no certificates in it", "welcome.txt: no certificates in it" in out and "exit status 1" in out, out)
        out = plain(m.run("fetch --ca", timeout=20))
        t.check("--ca wants a file", "usage: fetch" in out, out)

        out = plain(m.run("%s -i %s/moved" % (fetch, base), timeout=20))
        t.check("a redirect over TLS", "302" in out and "200 OK" in out and out.endswith("hello from the host"), out)
        out = plain(m.run("fetch -k -i %s/moved" % base, timeout=20))
        t.check("and with -k, one warning for the one server", out.endswith("hello from the host") and out.count(warning) == 1, out)
        out = plain(m.run("%s %s/rel/a" % (fetch, base), timeout=20))
        t.check("a relative redirect stays on TLS", "relative ok" in out and warning not in out, out)
        out = plain(m.run("%s %s/same" % (fetch, base), timeout=20))
        t.check("so does one that names only the host", "hello from the host" in out, out)
        out = plain(m.run("%s %s/secure" % (fetch, url), timeout=20))
        t.check("a redirect from http to https, checked", "hello from the host" in out and warning not in out, out)
        out = plain(m.run("fetch %s/secure" % url, timeout=20))
        t.check("and refused when the roots do not know the server", "is not signed by anyone we trust" in out
                and "exit status 1" in out, out)
        out = plain(m.run("%s %s/abs" % (fetch, base), timeout=20))
        t.check("and from https to http", "hello from the host" in out, out)
        out = plain(m.run("%s %s/loop" % (fetch, base), timeout=30))
        t.check("a redirect loop over TLS gives up", "too many redirects" in out and "exit status 1" in out, out)
        out = plain(m.run("%s %s/nope" % (fetch, base), timeout=20))
        t.check("a 404 over TLS", "HTTP 404" in out and "exit status 1" in out, out)
        out = plain(m.run("%s -o sbig.bin %s/big.bin" % (fetch, base), timeout=60))
        t.check("300000 bytes over TLS", "saved 300000 bytes to sbig.bin" in out, out)
        out = plain(m.run("cmp sbig.bin big.bin", timeout=30))
        t.check("the same bytes as over plain HTTP", out.strip() == "", out)
        m.run("rm sbig.bin")
        aes_work = tempfile.mkdtemp(prefix="ringtail-aes-")
        try:
            aes = _aes_only_server(pems["main"], aes_work)
            if aes:
                out = plain(m.run("%s -o abig.bin https://10.0.2.2:%d/big.bin" % (fetch, aes[1]), timeout=90))
                t.check("a server that takes only AES-128-GCM: 300000 bytes", "saved 300000 bytes to abig.bin" in out, out)
                out = plain(m.run("cmp abig.bin big.bin", timeout=30))
                t.check("the same bytes again", out.strip() == "", out)
                m.run("rm abig.bin")
                aes[0].kill()
            else:
                print("  (no openssl s_server, skipping the AES-128-GCM test)")
        finally:
            shutil.rmtree(aes_work)
        out = plain(m.run("%s %s/short" % (fetch, base), timeout=20))
        t.check("a short body over TLS", "closed after 11 of 100 bytes" in out and "exit status 1" in out, out)
        http11_tests(m, t, base, fetch)
        out = plain(m.run("%s -o cut.txt %s/cut" % (fetch, base), timeout=20))
        t.check("a body with no stated length, and a connection that just stops, is not trusted",
                "stopped before the server said it was done" in out and "exit status 1" in out, out)
        t.check("and is not kept", "no such file" in plain(m.run("cat cut.txt")))

        for (srv, port), what, want in zip(others, ["a server that only speaks TLS 1.2", "a server that speaks plain HTTP",
                                                    "a server that sends nonsense", "a server that hangs up"],
                                           ["does not speak TLS 1.3", "did not answer with TLS", "something TLS does not allow",
                                            "closed in the middle"]):
            out = plain(m.run("%s -o no.txt https://10.0.2.2:%d/" % (fetch, port), timeout=20))
            t.check(what + " is told apart", want in out and "exit status 1" in out and warning not in out, out)
        t.check("none of them left a file", "no such file" in plain(m.run("cat no.txt")))
        out = plain(m.run("%s %s/hello.txt" % (fetch, base), timeout=20))
        t.check("and fetch still works after all that", out.endswith("hello from the host"), out)
        m.run("rm ca.pem")
    finally:
        sweb.shutdown()
        sweb.server_close()
        for srv, _ in others + [rsa, wrong_name, expired]:
            srv.close()


def tcp_tests(m, t):
    """fetch and nc against servers on the host, which the guest
    reaches as 10.0.2.2."""
    web = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Http)
    web.agents = []
    threading.Thread(target=web.serve_forever, daemon=True).start()
    url = "10.0.2.2:%d" % web.server_port
    try:
        out = plain(m.run("fetch http://%s/hello.txt" % url, timeout=20))
        t.check("fetch gets a page", out.strip() == "hello from the host", out)
        t.check("and says who it is", web.agents and web.agents[-1].startswith("fetch (RingTail OS"), str(web.agents))
        out = plain(m.run("fetch -i %s/moved" % url, timeout=20))
        t.check("fetch follows a redirect, -i shows both responses",
                "302" in out and "200 OK" in out and out.endswith("hello from the host"), out)
        out = plain(m.run("fetch %s/rel/a" % url, timeout=20))
        t.check("a relative redirect", "relative ok" in out, out)
        out = plain(m.run("fetch %s/abs" % url, timeout=20))
        t.check("an absolute redirect", "hello from the host" in out, out)
        out = plain(m.run("fetch %s/loop" % url, timeout=20))
        t.check("a redirect loop gives up", "too many redirects" in out and "exit status 1" in out, out)
        out = plain(m.run("fetch %s/nope" % url, timeout=20))
        t.check("fetch reports 404", "HTTP 404" in out and "exit status 1" in out, out)
        out = plain(m.run("fetch %s/short" % url, timeout=20))
        t.check("fetch notices a short body", "closed after 11 of 100 bytes" in out, out)
        out = plain(m.run("fetch ftp://%s/" % url))
        t.check("fetch refuses other schemes", "only http and https" in out, out)
        out = plain(m.run("fetch -o big.bin %s/big.bin" % url, timeout=60))
        t.check("fetch -o saves a big file", "saved 300000 bytes to big.bin" in out, out)
        out = plain(m.run("wc big.bin"))
        t.check("all of it", "300000 big.bin" in out, out)
        http11_tests(m, t, "http://" + url)
        tls_tests(m, t, web)

        def counts(c):
            r = re.search(r"(\d+)\s+(\d+)\s+(\d+)", plain(m.run(c, timeout=60)))
            return (int(r.group(1)), int(r.group(3))) if r else None

        def lines_of(data):
            parts = data.split(b"\n")
            return [p + b"\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])

        def shape(data):
            return (data.count(b"\n"), len(data))

        ls = lines_of(BIG)
        # The tail of a file bigger than the 64 KB tail keeps before trimming.
        for c, want in [("tail -c 100 big.bin | wc", BIG[-100:]),
                        ("tail -c 70000 big.bin | wc", BIG[-70000:]),
                        ("tail -c +299901 big.bin | wc", BIG[299900:]),
                        ("tail -n 5 big.bin | wc", b"".join(ls[-5:])),
                        ("tail -n 1000 big.bin | wc", b"".join(ls[-1000:])),
                        ("tail -n +1100 big.bin | wc", b"".join(ls[1099:])),
                        ("head -c 70000 big.bin | wc", BIG[:70000]),
                        ("head -n 1000 big.bin | wc", b"".join(ls[:1000]))]:
            t.check(c + " counts what it should", counts(c) == shape(want), f"{counts(c)} != {shape(want)}")
        sortuniq_tests(m, t, url)
        coreutils_tests(m, t)
        flags_tests(m, t)
        grepwc_tests(m, t)
        gnu_text_tests(m, t)
        tools_tests(m, t, url)
        find_tests(m, t, url)
        out = plain(m.run("fetch -o gone.bin %s/short" % url, timeout=20))
        out = plain(m.run("cat gone.bin"))
        t.check("a failed -o leaves no file", "no such file" in out, out)

        # httpd, served and fetched on the guest's own loopback.
        for c in ["mkdir www", "mkdir www/sub", "write www/hi.txt hello", "write www/sub/index.html indexed page",
                  'cp www/hi.txt "www/My Notes.txt"', "cp big.bin www/big.bin"]:
            m.run(c, timeout=20)
        out = m.run("run httpd -q www &")
        srv = re.search(r"\[(\d+)\] started", out)
        t.check("httpd starts in the background", srv, out)
        time.sleep(1)
        get = lambda path, opts="": plain(m.run(f"fetch {opts} 127.0.0.1:8080{path}", timeout=30))
        t.check("httpd serves a file", get("/hi.txt").strip() == "hello", get("/hi.txt"))
        out = get("/hi.txt", "-i")
        t.check("with its length and type", "Content-Length: 6" in out and "text/plain" in out, out)
        out = get("/sub/")
        t.check("a directory gives its index.html", out.strip() == "indexed page", out)
        out = get("/")
        t.check("or a listing, with names escaped for URLs",
                'href="My%20Notes.txt"' in out and 'href="sub/"' in out and "(6 bytes)" in out, out)
        out = get("/sub")
        t.check("a directory without a slash is sent to the one with it", "indexed page" in out, out)
        t.check("a name with a space", get("/My%20Notes.txt").strip() == "hello", get("/My%20Notes.txt"))
        t.check("a missing file is a 404", "HTTP 404" in get("/nope"), get("/nope"))
        for bad in ["/../x", "/%2e%2e/x", "/sub%2f..%2fhi.txt", "/sub/../hi.txt"]:
            out = get(bad)
            t.check(f"{bad} is refused", "HTTP 403" in out and "hello" not in out, out)
        out = get("/big.bin", "-o got.bin")
        out = plain(m.run("wc got.bin", timeout=20))
        t.check("httpd serves a 300000-byte file whole", "300000 got.bin" in out, out)
        m.run("write req.txt POST", timeout=10)
        out = plain(m.run("nc 127.0.0.1 8080 < req.txt", timeout=30))
        t.check("garbage is a 400", "400 Bad Request" in out, out)
        out = get("/hi.txt")
        t.check("and httpd still answers after it", out.strip() == "hello", out)
        if srv:
            out = m.run("kill " + srv.group(1))
            t.check("httpd can be killed", "Killed task" in out, out)
        for c in ["rm www/sub/index.html", "rmdir www/sub", "rm www/hi.txt", 'rm "www/My Notes.txt"', "rm www/big.bin",
                  "rmdir www", "rm got.bin", "rm req.txt"]:
            out = m.run(c, timeout=20)
            t.check(f"{c} (cleaning up)", plain(out).strip() == "", out)
    finally:
        web.shutdown()
        web.server_close()

    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(20)
    got = []
    th = threading.Thread(target=_echo_once, args=(srv, got), daemon=True)
    th.start()
    m.run("echo shout this > nc.txt")
    out = plain(m.run("nc -v 10.0.2.2 %d < nc.txt" % srv.getsockname()[1], timeout=20))
    th.join(5)
    srv.close()
    t.check("nc sends stdin and prints the answer", b"".join(got) == b"shout this\n" and "SHOUT THIS" in out
            and "Connected to 10.0.2.2" in out, out)
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    out = plain(m.run("nc 10.0.2.2 %d" % port, timeout=20))
    t.check("nc to a closed port", "connection refused" in out and "exit status 1" in out, out)
    out = plain(m.run("net"))
    t.check("net lists TCP", "TCP:" in out and "opened" in out, out)


def _rows(m):
    """The screen, as its 25 rows without trailing blanks."""
    return [m.screen_row(i)[0].rstrip() for i in range(25)]


def text_tests(m, t):
    """head, tail and more, on 30 lines of help (all of it, then its first
    eight lines again) and a few small files."""
    def run(c, timeout=20):  # output as it is, indentation included
        return re.sub(r"\x1b\[[0-9;]*m", "", m.run(c, timeout=timeout)).replace("\r", "").rstrip()

    # help leaves its three footer lines off when it is redirected, so
    # the file is the reference, not what help prints on the screen.
    t.check("help > h.txt (setting up)", run("help > h.txt") == "")
    t.check("help | head -n 8 >> h.txt (setting up)", run("help | head -n 8 >> h.txt") == "")
    text = run("cat h.txt")
    lines = text.split("\n")
    full = text + "\n"
    t.check("the text to page is 30 lines", len(lines) == 30 and all(l == l.rstrip() for l in lines), str(len(lines)))
    for c in ["head -n 24 h.txt > h24.txt", "head -c 5 h.txt > nonl.txt",
              "write w150.txt " + "x" * 150, "write w80.txt " + "y" * 80]:
        t.check(f"{c[:30]} (setting up)", run(c) == "", c)
    j = "\n".join

    # head
    t.check("head -n 3", run("head -n 3 h.txt") == j(lines[:3]))
    t.check("head -n3 and -3 are the same", run("head -n3 h.txt") == j(lines[:3]) == run("head -3 h.txt"))
    t.check("head defaults to ten lines", run("head h.txt") == j(lines[:10]))
    t.check("head -c counts bytes", run("head -c 12 h.txt") == text[:12].rstrip())
    t.check("head -n 0 prints nothing", run("head -n 0 h.txt") == "")
    t.check("head -n more than there is prints it all", run("head -n 500 h.txt") == text)
    t.check("head names its files when there are two",
            run("head -n 2 h.txt h24.txt") == "==> h.txt <==\n" + j(lines[:2]) + "\n\n==> h24.txt <==\n" + j(lines[:2]))
    t.check("and -q does not", run("head -q -n 1 h.txt h24.txt") == lines[0] + "\n" + lines[0])
    t.check("head reads a pipe", run("cat h.txt | head -n 2") == j(lines[:2]))
    t.check("and - means standard input", run("cat h.txt | head -n 1 -") == lines[0])
    out = run("head -n -1 h.txt")
    t.check("head rejects a negative count", "invalid number of lines: '-1'" in out and "exit status 2" in out, out)
    out = run("head nosuch")
    t.check("head says when a file is missing", "head: nosuch: no such file" in out and "exit status 1" in out, out)
    out = run("head -z h.txt")
    t.check("head rejects an unknown option", "usage: head" in out and "exit status 2" in out, out)

    # tail
    t.check("tail -n 3", run("tail -n 3 h.txt") == j(lines[-3:]))
    t.check("tail -3 is the same", run("tail -3 h.txt") == j(lines[-3:]))
    t.check("tail defaults to ten lines", run("tail h.txt") == j(lines[-10:]))
    t.check("tail -n +K starts at line K", run("tail -n +%d h.txt" % (len(lines) - 1)) == j(lines[-2:]))
    t.check("tail -c counts bytes", run("tail -c 20 h.txt") == full[-20:].rstrip())
    t.check("tail -c +K starts at byte K", run("tail -c +%d h.txt" % (len(full) - 9)) == full[-10:].rstrip())
    t.check("tail -n 0 prints nothing", run("tail -n 0 h.txt") == "")
    t.check("tail -n more than there is prints it all", run("tail -n 500 h.txt") == text)
    t.check("a last line with no newline counts as a line", run("tail -n 1 nonl.txt") == text[:5] == run("tail -n +1 nonl.txt"))
    t.check("tail -c on it", run("tail -c 3 nonl.txt") == text[2:5])
    t.check("tail names its files when there are two",
            run("tail -n 1 h.txt h24.txt") == "==> h.txt <==\n" + lines[-1] + "\n\n==> h24.txt <==\n" + lines[23])
    t.check("tail reads a pipe", run("cat h.txt | tail -n 2") == j(lines[-2:]))
    t.check("head and tail together", run("head -n 5 h.txt | tail -n 1") == lines[4])
    out = run("tail -f h.txt")
    t.check("tail has no -f", "usage: tail" in out and "exit status 2" in out, out)
    out = run("tail nosuch")
    t.check("tail says when a file is missing", "tail: nosuch: no such file" in out and "exit status 1" in out, out)

    # more: the keys come from the terminal, so a pipe can feed it
    first = sum(len(l) + 1 for l in lines[:24])
    pct = lambda n: "--More--(%d%%)" % (sum(len(l) + 1 for l in lines[:n]) * 100 // len(full))

    def start(c, want="--More--"):
        m.send(c + "\r")
        return m.wait_row(24, want, 10)

    row = start("more h.txt", pct(24))
    t.check("more stops after a screenful and says how far it has got", row.strip() == pct(24), row)
    rows = _rows(m)
    t.check("the screen holds exactly the first 24 lines", rows[:24] == lines[:24], j(rows))
    m.send("\r")
    row = m.wait_row(24, pct(25), 10)
    rows = _rows(m)
    t.check("Enter shows one more line", row.strip() == pct(25) and rows[:24] == lines[1:25], j(rows))
    m.send("\x1b[B")
    row = m.wait_row(24, pct(26), 10)
    t.check("and so does Down", row.strip() == pct(26) and _rows(m)[:24] == lines[2:26], row)
    m.send(" ")
    m.expect(PROMPT, 10)
    rows = _rows(m)
    t.check("Space shows the next screen, and more ends with the file", rows[:24] == lines[6:30] and rows[24].startswith("ringtail:/>"), j(rows))
    start("more h.txt", pct(24))
    m.send("\x1b[6~")
    m.expect(PROMPT, 10)
    t.check("PgDn does the same", _rows(m)[:24] == lines[6:30])
    start("more h.txt", pct(24))
    m.send("q")
    m.expect(PROMPT, 10)
    rows = _rows(m)
    t.check("q quits and leaves the page on the screen", rows[:24] == lines[:24] and rows[24].startswith("ringtail:/>"), j(rows))
    start("more h.txt", pct(24))
    m.send("\x03")
    m.expect(PROMPT, 10)
    rows = _rows(m)
    t.check("Ctrl+C at the prompt quits the same way, without wiping the screen",
            rows[:24] == lines[:24] and not any("^C" in r for r in rows), j(rows))
    row = start("cat h.txt | more")
    t.check("more reads a pipe, and without a size gives no percentage", row.strip() == "--More--", row)
    t.check("from the first line", _rows(m)[:24] == lines[:24])
    m.send("q")
    m.expect(PROMPT, 10)
    t.check("quitting does not leave the pipeline running", "more" not in run("tasks"))
    m.send("run sleep 100 | more\r")
    time.sleep(1)
    m.send("\x03")
    m.expect(PROMPT, 10)
    out = run("tasks")
    t.check("Ctrl+C while the input is stalled ends the pipeline", "sleep" not in out and "more" not in out, out)
    out = run("more h.txt | wc")
    t.check("more copies when its output is a pipe", re.search(r"^\s*%d\s+\d+\s+%d$" % (len(lines), len(full)), out), out)
    t.check("or a file", run("more h.txt > out.txt") == "" and
            re.search(r"^\s*%d\s+\d+\s+%d " % (len(lines), len(full)), run("wc out.txt")))
    out = run("more nosuch")
    t.check("more says when a file is missing", "more: nosuch: no such file" in out and "exit status 1" in out, out)
    out = run("more -x")
    t.check("more rejects an unknown option", "usage: more" in out and "exit status 2" in out, out)
    out = run("more")
    t.check("and has nothing to show without a file or a pipe", "usage: more" in out and "exit status 2" in out, out)

    # Counting rows the way the console lays them out: a 150-character
    # line takes two, and an 80-character one only one.
    for name, body in [("w150.txt", [80, 70]), ("w80.txt", [80])]:
        names = [name] * 10
        want = []
        for k in range(10):
            want += ([""] if k else []) + ["==> %s <==" % name] + ["x" * n if name == "w150.txt" else "y" * n for n in body]
        row = start("more " + " ".join(names))
        rows = _rows(m)
        t.check(f"more counts wrapped rows ({name} x10)", rows[:24] == want[:24] and row.startswith("--More--"), j(rows))
        m.send("q")
        m.expect(PROMPT, 10)

    for c in ["rm h.txt", "rm h24.txt", "rm nonl.txt", "rm out.txt", "rm w150.txt", "rm w80.txt"]:
        t.check(f"{c} (cleaning up)", run(c) == "", c)


def netconf_tests(m, t):
    """Setting the address by hand, and going back to DHCP."""
    out = m.run("net static 10.0.2.20/24 10.0.2.2 10.0.2.3")
    t.check("net static says nothing when it works", plain(out) == "", repr(out))
    out = plain(m.run("net"))
    t.check("and the address is the one given", "Address:  10.0.2.20/24, gateway 10.0.2.2\n" in out and
            "DHCP:     off (the address was set by hand)" in out and "DNS:      10.0.2.3\n" in out, out)
    out = m.run("ping -c 1 10.0.2.2", timeout=20)
    t.check("and it works", "1 received" in out, out)
    out = m.run("net static 10.0.2.21/24 none")
    out = m.run("net")
    t.check("none means no gateway, and no DNS server given means none",
            "10.0.2.21/24, no gateway" in out and "DNS:      none" in out, out)
    out = m.run("ping -c 1 8.8.8.8")
    t.check("so nothing off the network can be reached", "seq=1: network unreachable" in out, out)
    out = plain(m.run("host a.test"))
    t.check("and no name looked up", "host: a.test: no DNS server to ask (see net)" in out, out)
    for bad in ["10.0.2.255/24", "10.0.2.0/24", "10.0.2.20/24 10.0.3.1", "10.0.2.20/24 10.0.2.20",
                "224.0.0.1/24", "10.0.2.20/24 10.0.2.2 224.0.0.1"]:
        out = m.run("net static " + bad)
        t.check(f"net static {bad} does not fit", "net: those addresses do not fit together" in out, out)
    for bad in ["10.0.2.20", "10.0.2.20/31", "10.0.2.20/0", "10.0.2.20/24 x", "10.0.2.20/24 10.0.2.2 10.0.2.3 more",
                "frob", "dhcp now"]:
        out = m.run("net " + ("static " + bad if not bad.startswith(("frob", "dhcp")) else bad))
        t.check(f"net {bad} is a usage error", "Usage: net" in out, out)
    t.check("and none of those changed anything", "10.0.2.21/24, no gateway" in m.run("net"))
    out = m.run("net dhcp", timeout=20)
    t.check("net dhcp asks again, for the address DHCP gave before",
            "Asking DHCP... 10.0.2.15/24, gateway 10.0.2.2 from 10.0.2.2" in out, out)
    out = plain(m.run("net"))
    t.check("and is back where it started", "Address:  10.0.2.15/24, gateway 10.0.2.2\n" in out and
            "DHCP:     from 10.0.2.2" in out and "DNS:      10.0.2.3\n" in out, out)


def short_boots(iso, disk, work, t):
    """The other e1000 models QEMU has, and a machine with no card at all."""
    for model, banner, script in [
            ("e1000-82545em", "Network: e1000 82545EM", [("ping -c 1 10.0.2.2", "1 received")]),
            ("e1000-82544gc", "Network: e1000 82544GC", [("ping -c 1 10.0.2.2", "1 received")]),
            ("none", "Network: no card", [("net", "No network card."),
                                          ("ping 10.0.2.2", "ping: network is down"),
                                          ("ping -c 1 127.0.0.1", "1 received"),
                                          ("run udptest", "udptest: all"),
                                          ("run tcptest", "tcptest: all"),
                                          ("host a.test", "no DNS server to ask"),
                                          ("net dhcp", "net: network is down")])]:
        sub = os.path.join(work, model)
        os.mkdir(sub)
        shutil.copy(disk, os.path.join(sub, "disk.img"))
        vm = Machine(iso, os.path.join(sub, "disk.img"), sub,
                     net=["-nic", "none" if model == "none" else "user,model=" + model])
        try:
            boot = vm.expect(PROMPT, 30)
            t.check(f"[{model}] boots and says so", banner in boot, boot)
            for cmd, want in script:
                out = vm.run(cmd, timeout=20)
                t.check(f"[{model}] {cmd}", want in out, out)
        except (TimeoutError, EOFError) as e:
            t.check(f"[{model}] session finished", False, str(e))
        finally:
            vm.qemu.kill()
            vm.qemu.wait()
            with open(os.path.join(sub, "serial.log"), "wb") as f:
                f.write(vm.log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/ringtail.iso")
    ap.add_argument("--disk", default="build/test-disk.img")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="ringtail-test-")
    disk = os.path.join(work, "disk.img")
    shutil.copy(args.disk, disk)
    global HOST_FILE
    if shutil.which("mcopy"):
        # A long name written by another implementation, for the guest to read.
        src = os.path.join(work, "host.txt")
        with open(src, "w") as f:
            f.write("written by mtools\n")
        HOST_FILE = subprocess.run(["mcopy", "-i", disk, src, "::From the host, with love.txt"]).returncode == 0
    t = Runner()
    m = Machine(args.iso, disk, work)
    try:
        tests(m, t)
    except (TimeoutError, EOFError) as e:
        t.check("session finished", False, str(e))
    finally:
        if m.qemu.poll() is None:
            m.qemu.kill()
            m.qemu.wait()
        with open(os.path.join(work, "serial.log"), "wb") as f:
            f.write(m.log)

    if shutil.which("fsck.fat"):
        r = subprocess.run(["fsck.fat", "-n", disk], capture_output=True, text=True)
        t.check("fsck.fat finds the disk consistent", r.returncode == 0, r.stdout + r.stderr)
    if shutil.which("mtype"):
        r = subprocess.run(["mtype", "-i", disk, "::KEEP.TXT"], capture_output=True, text=True)
        t.check("another FAT implementation reads our file", "survives a power cycle" in r.stdout,
                r.stdout + r.stderr)

        r = subprocess.run(["mtype", "-i", disk, "::KEEPDIR/DEEP.TXT"], capture_output=True, text=True)
        t.check("and our subdirectory", "nested and kept" in r.stdout, r.stdout + r.stderr)
        r = subprocess.run(["mtype", "-i", disk, "::My Docs/Back To A Long One.txt"], capture_output=True, text=True)
        t.check("and our long names", "long names work" in r.stdout, r.stdout + r.stderr)
        r = subprocess.run(["mdir", "-i", disk, "::My Docs"], capture_output=True, text=True)
        now = utcnow()
        days = {(now - datetime.timedelta(days=k)).strftime("%Y-%m-%d") for k in (0, 1)}
        t.check("and our timestamps", any(d in r.stdout for d in days), r.stdout + r.stderr)

    # A second boot, on a network whose only other host is this script.
    import net_peer
    peer_dir = os.path.join(work, "peer")
    os.mkdir(peer_dir)
    peer_disk = os.path.join(peer_dir, "disk.img")
    shutil.copy(args.disk, peer_disk)
    pm = None

    def make_machine(net):
        nonlocal pm
        pm = Machine(args.iso, peer_disk, peer_dir, net=net)
        return pm
    try:
        net_peer.peer_tests(make_machine, PROMPT, t)
    except (TimeoutError, EOFError, RuntimeError) as e:
        t.check("[peer] session finished", False, str(e))
    finally:
        if pm:
            if pm.qemu.poll() is None:
                pm.qemu.kill()
                pm.qemu.wait()
            with open(os.path.join(peer_dir, "serial.log"), "wb") as f:
                f.write(pm.log)

    short_boots(args.iso, args.disk, work, t)

    print(f"\n{t.passed} passed, {len(t.failed)} failed")
    print(f"serial log: {os.path.join(work, 'serial.log')}")
    if not args.keep and not t.failed:
        shutil.rmtree(work)
    sys.exit(1 if t.failed else 0)


if __name__ == "__main__":
    main()

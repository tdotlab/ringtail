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
    t.check("boots to the shell", "RingTail OS v0.23.0" in boot, boot)
    t.check("finds the FAT16 disk", re.search(r"Disk: 16 MB FAT16, \d+ files", boot), boot)

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
    out = m.run("echo  two   spaces")
    t.check("echo keeps the spacing", "two   spaces" in out, out)
    out = m.run("exit")
    t.check("exit gets a fresh shell from init", "starting a new one" in out, out)
    out = m.run("version")
    t.check("and the new shell works", "0.23.0" in out, out)

    for prog in ["badptr", "kptr", "filedemo", "seekdemo", "listdemo", "bss", "stack", "heaptest", "pipetest", "fgspawn",
                 "udptest"]:
        out = m.run("run " + prog, timeout=30)
        t.check(f"{prog} self-checks pass", f"{prog}: all" in out and "FAIL" not in out, out)
    out = m.run("run nullds")
    t.check("null data segments cannot hurt a syscall", "worked" in out, out)

    # The network, through QEMU's user network: 10.0.2.2 is its gateway
    # and DHCP server, and answers ARP and pings; 10.0.2.3 is its DNS
    # server; nobody else on 10.0.2.0/24 answers.
    t.check("finds the network card", "Network: e1000 82540EM 52:54:00:12:34:56\n" in boot, boot)
    t.check("and gets an address from DHCP before the shell starts",
            "Network: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 (DHCP)\nType 'help'" in boot, boot)
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
            re.search(r"Pings:    [4-9] answered; \d\d packets through loopback", out), out)
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
    t.check("a built-in pipes into a program", "WELCOME.TXT" in out and "HELLO" not in out, out)
    t.check("and leaves out what is only for the screen", "files," not in out and "\x1b[90m" not in out, out)
    out = m.run("help | grep poweroff")
    t.check("help can be searched", "poweroff" in out and "history" not in out, out)
    m.run("echo one  two > out.txt")
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
    out = m.run("hello | ls")
    t.check("a built-in cannot come after |", "built into the shell" in out, out)
    out = m.run("hello >")
    t.check("> needs a file name", "Missing file name" in out, out)
    out = m.run("ls < out.txt")
    t.check("built-ins do not take <", "do not read input" in out, out)
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
    t.check("touch of an existing file is an error", "already exists" in out, out)
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
    out = m.run("mv docs/m2.txt docs/a.txt")
    t.check("mv onto an existing file is refused", "already exists" in out, out)
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
    t.check("Tab completes a built-in", "RingTail OS v0.23.0" in out, out)
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
    t.check("gauge -1 shows the version and clock", "RingTail OS v0.23.0" in out and " up " in out, out)
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
    t.check("gauge draws a title row", first.startswith(" Gauge  RingTail OS v0.23.0"), repr(first))
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
                      ("loop.test", "too many aliases"), ("big.test", "the answer is too big for UDP, and there is no TCP"),
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

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
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PROMPT = b"ringtail> "


class Machine:
    def __init__(self, iso, disk, workdir):
        self.dir = workdir
        self.serial_path = os.path.join(workdir, "serial.sock")
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-cdrom", iso,
             "-drive", f"file={disk},format=raw,if=ide,index=0",
             "-boot", "d", "-m", "64", "-display", "none",
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
        out = self.expect(re.escape(PROMPT), timeout)
        out = out[:-len(PROMPT)].replace("\r\n", "\n")
        first_nl = out.find("\n")
        return out[first_nl + 1:] if first_nl >= 0 else ""

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


def tests(m, t):
    boot = m.expect(re.escape(PROMPT), 30).replace("\r\n", "\n")
    t.check("boots to the shell", "RingTail OS v0.12.0" in boot, boot)
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

    for prog in ["badptr", "kptr", "filedemo", "seekdemo", "listdemo", "bss", "stack", "heaptest"]:
        out = m.run("run " + prog, timeout=30)
        t.check(f"{prog} self-checks pass", f"{prog}: all" in out and "FAIL" not in out, out)
    out = m.run("run nullds")
    t.check("null data segments cannot hurt a syscall", "worked" in out, out)

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
    out = m.expect(re.escape(PROMPT), 5)
    t.check("Ctrl+C stops a running program", "^C" in out and time.time() - start < 2, out)

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
    t.check("the shell refuses to kill itself", "will not kill itself" in out or "no such" in out, out)

    # Interactive stdin.
    m.send("run readline\r")
    m.expect(rb"> ", 5)
    m.send("abc def\r")
    out = m.expect(rb"got .*\n", 5)
    t.check("a program reads a line from the terminal", 'got "abc def"' in out, out)
    m.expect(rb"> ", 5)
    m.send("\x04")
    out = m.expect(re.escape(PROMPT), 5)
    t.check("Ctrl+D is end of file", "readline: 1 lines" in out, out)

    # Mouse input wakes a waiting program.
    m.send("run waitdemo\r")
    m.expect(rb"Waiting for input", 5)
    time.sleep(0.3)
    m.monitor_cmd("mouse_move 5 5")
    out = m.expect(re.escape(PROMPT), 5)
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
    t.check("bad names are rejected", "invalid" in out, out)
    m.run("rm note.txt")
    out = m.run("cat note.txt")
    t.check("rm deletes", "no such file" in out, out)
    m.run("write keep.txt this survives a power cycle")

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
    out = m.expect(re.escape(PROMPT), 5)
    t.check("Ctrl+C still kills a raw program", "^C" in out, out)
    out = m.run("echo cooked")
    t.check("the terminal is cooked again afterwards", "cooked" in out, out)

    m.send("run rawkeys nosig\r")
    m.expect(rb"raw mode on", 5)
    m.send("\x03")
    out = m.expect(rb"keys: [^\n]*\n", 5)
    t.check("with TTY_NOSIG, Ctrl+C is a byte", "keys: 03" in out, out)
    time.sleep(1.3)
    m.send("\x03\x03\x03")
    out = m.expect(re.escape(PROMPT), 5)
    t.check("three quick Ctrl+C still kill it", "^C" in out, out)

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
    out = m.expect(re.escape(PROMPT), 5)
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
    t.check("an untitled document is saved as a lemur", "Saved zoo.LEM" in m.wait_row(23, "Saved"))
    m.send("\x11")
    m.expect(re.escape(PROMPT), 5)
    out = m.run("cat zoo.lem")
    t.check("and the file is there", out.count("lemur") == 20, repr(out))

    # History recall with the Up arrow.
    m.run("echo first")
    m.send("\x1b[A\r")
    out = m.expect(re.escape(PROMPT), 5)
    t.check("Up arrow recalls the last command", out.count("first") >= 2, out)

    # The desktop: graphics mode, then Esc back to text.
    m.send("run gui\r")
    # QEMU shows mode 13h pixel-doubled, so 320x200 dumps as 640x400.
    size = None
    for _ in range(30):
        time.sleep(0.2)
        size = m.screen_size()
        if size == (640, 400):
            break
    t.check("gui switches to mode 13h graphics", size == (640, 400), f"screen is {size}")
    m.monitor_cmd("sendkey esc")
    m.expect(re.escape(PROMPT), 5)
    t.check("Esc leaves the gui and text mode returns", m.screen_size() == (720, 400))

    out = m.run("mem")
    t.check("mem", "Frames:" in out and "Heap:" in out, out)

    m.send("reboot\r")
    out = m.expect(re.escape(PROMPT), 30)
    t.check("reboot comes back up", "RingTail OS" in out, out)
    out = m.run("cat keep.txt")
    t.check("files survive a reboot", "survives a power cycle" in out, out)
    m.send("poweroff\r")
    m.close()
    t.check("poweroff stops the machine", m.qemu.returncode is not None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/ringtail.iso")
    ap.add_argument("--disk", default="build/test-disk.img")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="ringtail-test-")
    disk = os.path.join(work, "disk.img")
    shutil.copy(args.disk, disk)
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

    print(f"\n{t.passed} passed, {len(t.failed)} failed")
    print(f"serial log: {os.path.join(work, 'serial.log')}")
    if not args.keep and not t.failed:
        shutil.rmtree(work)
    sys.exit(1 if t.failed else 0)


if __name__ == "__main__":
    main()

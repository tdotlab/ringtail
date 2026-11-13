#!/usr/bin/env python3
"""Writes shasum_cases.h: lines of a checksum list and what GNU sha256sum -c
makes of each. Needs GNU sha256sum. The generated file is checked in, so
test_shasum needs nothing but a compiler; rerun this only to change the cases.

GNU settles, from the first line that shows it, whether the lines of a run
have a mode character after the blank that follows the hash, and holds every
later line to it. So a case may have a line before it (from the small
PREFIXES table, which also goes into the header) and GNU is asked about the
second line.

How GNU is asked: the line goes into a list with a good line after it
("HASH  a", where a holds "hello\\n"). If the list's output has two verdicts,
the line named a file (the name is printed back, so it is known, and the
verdict says whether the hash was the hash of "hello\\n" when the name is
a); if it has a warning about a badly formatted line, it was improper;
otherwise it was skipped. The program sha256sum is run under the name
sha256, so its messages read as ours do."""
import hashlib, os, random, subprocess, sys, tempfile

random.seed(5895)
H = hashlib.sha256(b"hello\n").hexdigest()
h = H.encode()

D = tempfile.mkdtemp(prefix="shacase-")
BIN = os.path.join(D, "sha256")
os.symlink(subprocess.run(["which", "sha256sum"], capture_output=True, text=True).stdout.strip(), BIN)
open(os.path.join(D, "a"), "wb").write(b"hello\n")

def verdicts(data):
    open(os.path.join(D, "L"), "wb").write(data)
    p = subprocess.run([BIN, "-c", "-w", "L"], cwd=D, capture_output=True, env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"})
    sts = []
    for l in p.stdout.split(b"\n"):
        if l.endswith(b": OK"): sts.append((l[:-4], "OK"))
        elif l.endswith(b": FAILED open or read"): sts.append((l[:-len(b": FAILED open or read")], "OPEN"))
        elif l.endswith(b": FAILED"): sts.append((l[:-8], "FAILED"))
    return sts, p.stderr

def unshow(shown):
    if shown[:1] != b"\\": return shown
    body, name, i = shown[1:], bytearray(), 0     # the name was shown in backslash form
    while i < len(body):
        if body[i:i+1] == b"\\":
            name += {b"\\": b"\\", b"n": b"\n", b"r": b"\r"}[body[i+1:i+2]]; i += 2
        else:
            name += body[i:i+1]; i += 1
    return bytes(name)

def gnu(line, pre=None):
    """('skip',) | ('improper',) | (name, 'OK'|'FAILED'|'OPEN'), for line when it comes after pre"""
    good = h + b"  a\n"
    base = 0
    if pre is not None:
        base = len(verdicts(pre + b"\n")[0])      # a line alone makes at most one verdict
    sts, err = verdicts((pre + b"\n" if pre is not None else b"") + line + b"\n" + good)
    if (b": %d: improperly" % (2 if pre is not None else 1)) in err: return ("improper",)
    if len(sts) >= base + 2:
        shown, st = sts[base]
        return (unshow(shown), st)
    return ("skip",)

def hashes():
    r = random.random()
    x = h
    if r < 0.12: x = h.upper()
    elif r < 0.2: x = bytes(c ^ 0x20 if (random.random() < .5 and 97 <= c <= 102) else c for c in h)
    elif r < 0.27: x = h[:random.randrange(0, 64)]
    elif r < 0.34: x = h + random.choice([b"0", b"a", b"g", b"ab"])
    elif r < 0.42:
        k = random.randrange(64); x = h[:k] + random.choice([b"g", b"G", b" ", b"-", b"\xe9", b"x"]) + h[k+1:]
    elif r < 0.47: x = h[:63] + b"0"      # the right length, the wrong hash
    elif r < 0.55: x = bytes(random.choice(b"0123456789abcdefABCDEF") for _ in range(64))
    return x

BL = [b"", b"", b"", b" ", b"  ", b"\t"]
SEP = [b"\0", b"\0 ", b"\0*", b" \0", b"  ", b"  ", b"  ", b" *", b" ", b"\t", b" \t", b"\t ", b"   ", b"    ", b"  *", b" **", b"", b"*", b"\t\t", b"  \t", b" *\t"]
NAMES = [b"a\0b", b"\0a", b"a\\\0", b"a", b"a", b"a", b"", b" ", b"  ", b"*", b"**", b"*a", b" a", b"a ", b"a\t", b"\ta", b"a b", b"a\\n", b"a\\\\", b"a\\", b"\\", b"\\a", b"a\\r", b"a\\x", b"a\\x41",
         b"\\n", b"\\\\n", b"a)", b"(a", b"(a)", b"a) = x", b"a\r", b"a\r\r", b"\r", b"b", b"ab", b"a\xe9", b"#", b"# a", b"a #", b"A"]
TAILS = [b"", b"", b"", b"", b"", b"\r", b" ", b"\t", b"\r\r", b"x", b"\0", b"\0b"]

def line():
    """(the line, the hash in it)"""
    r = random.random()
    pre = random.choice(BL) + (b"\\" if random.random() < .25 else b"")
    hx = hashes()
    if r < 0.2:  # BSD
        a = random.choice([b"SHA256", b"SHA256", b"SHA256", b"sha256", b"SHA1", b"SHA512", b"SHA256 ", b"SHA256  ", b"SHA255", b"SHA25"])
        sp = random.choice([b" ", b" ", b"", b"  ", b"\t"])
        mid = random.choice([b") = ", b") = ", b") =", b")= ", b")=", b")  =  ", b") \t= ", b")", b") ", b") - ", b"))= ", b") =\t", b" = ", b"= ", b"x = ", b" =", b"] = "])
        return pre + a + sp + b"(" + random.choice(NAMES) + mid + hx + random.choice(TAILS), hx
    if r < 0.27:
        return random.choice([b"", b" ", b"\t", b"#", b"# x", b" #", b"##" + h, b"#" + h + b"  a", b"\r", b" \r", b"x", b"hello", b"a", b"-"]), hx
    return pre + hx + random.choice(SEP) + random.choice(NAMES) + random.choice(TAILS), hx

def c_str(b):
    out = []
    for x in b:
        if x == 0x22: out.append('\\"')
        elif x == 0x5c: out.append("\\\\")
        elif 0x20 <= x < 0x7f: out.append(chr(x))
        else: out.append("\\%03o" % x)
    return '"' + "".join(out) + '"'

# Lines that come before a case: ones that set the style either way, ones that
# set nothing, and some made lines (good or not).
PREFIXES = [h + b"  a", h + b" *a", h + b"   a", h + b"  *a", h + b"\0 a", h + b"  ab", b"\\" + h + b"  a\\n",
            h + b" a", h + b"\ta", h + b" \ta", h + b"\0a", h + b" ab", h + b"  ", h + b" *", b"\\" + h + b" b\\q",
            h + b" ", h + b"\t", h, b"", b"# x", b"SHA256 (a) = " + h, b"SHA256 (b) = " + h, h[:-1] + b"g  a"]
PREFIXES += [line()[0] for _ in range(20)]

# Lines of just the right length, and one short of it, in every dress.
EDGES = []
for pre in (b"", b" ", b"\\", b"\t\\"):
    for n in (62, 63, 64, 65, 66):
        for tail in (b"", b" ", b"  ", b" x", b"  x", b"\0", b"\0x", b"\r"):
            EDGES.append(pre + h[:n] + (b"0" * (n - 64) if n > 64 else b"") + tail)
for n in range(50, 64):
    EDGES.append(b"SHA256 (" + h[:n])
    EDGES.append(b"\\SHA256 (" + h[:n])
for n in range(0, 8):
    EDGES.append(b"SHA256 (" + h[:n])
    EDGES.append(b"SHA256 (a) = " + h[:64 - n])
    EDGES.append(b"SHA255 (a) = " + h)

N = int(sys.argv[1]) if len(sys.argv) > 1 else 6000
cases = []
kinds = {"skip": 0, "improper": 0, "line": 0}
for t in range(N + len(EDGES)):
    if t < len(EDGES):
        l, hx = EDGES[t], h
    else:
        l, hx = line()
    pi = -1 if t % 3 == 0 else random.randrange(len(PREFIXES))
    g = gnu(l, PREFIXES[pi] if pi >= 0 else None)
    if g == ("skip",): cases.append((l, 0, b"", 0, pi)); kinds["skip"] += 1
    elif g == ("improper",): cases.append((l, 1, b"", 0, pi)); kinds["improper"] += 1
    else:
        assert len(hx) == 64, (l, hx)
        cases.append((l, 2, g[0], l.rfind(hx), pi)); kinds["line"] += 1

with open("tests/shasum_cases.h", "w") as f:
    f.write("// Made by tests/gen_shasum_cases.py: a line of a checksum list and what GNU sha256sum -c made of it.\n")
    f.write("// kind: 0 skipped, 1 improperly formatted, 2 a line with this name; its hash is the 64 hex digits at hashoff.\n")
    f.write("// prefix: -1, or the index in shasum_prefixes of a line that came before it in the same run.\n")
    f.write("typedef struct { const char* line; unsigned linelen; } shasum_prefix_t;\n")
    f.write("static const shasum_prefix_t shasum_prefixes[] = {\n")
    for p in PREFIXES:
        f.write("    { %s, %d },\n" % (c_str(p), len(p)))
    f.write("};\n")
    f.write("typedef struct { const char* line; unsigned linelen; int kind; const char* name; unsigned namelen; unsigned hashoff; int prefix; } shasum_case_t;\n")
    f.write("static const shasum_case_t shasum_cases[] = {\n")
    for l, k, name, off, pi in cases:
        f.write("    { %s, %d, %d, %s, %d, %d, %d },\n" % (c_str(l), len(l), k, c_str(name), len(name), off, pi))
    f.write("};\n")
print(len(cases), "cases:", kinds)

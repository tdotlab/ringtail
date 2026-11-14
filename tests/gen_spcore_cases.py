#!/usr/bin/env python3
"""Writes spcore_cases.h: what GNU split made of option numbers (-l, -b, -a)
and the file names it wrote with the suffix options. Needs GNU split
(/usr/bin/split). The generated file is checked in, so test_spcore needs
nothing but a compiler; rerun this only to change the cases.

How the answers are found: a number is judged by running split on an empty
file (an error message or none), and where it is small enough the size is
checked on a real run too (the first piece of 2N+1 bytes or lines is N long).
The suffixes are the names split made (in the order of its --verbose
messages) for a file of one-line records, up to where it ran out of names."""
import os, re, subprocess, sys, tempfile

GNU = "/usr/bin/split"
ENV = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
MAX64 = (1 << 64) - 1

def run(args, data=b"", keep=False):
    d = tempfile.mkdtemp(prefix="spgen")
    with open(os.path.join(d, "f"), "wb") as f:
        f.write(data)
    p = subprocess.run([b"split", *args, b"f"], executable=GNU, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=ENV, timeout=300)
    names = [n for n in os.listdir(d) if n != "f"]
    return p, d, names

def cleanup(d, names):
    for n in names + ["f"]:
        os.unlink(os.path.join(d, n))
    os.rmdir(d)

def b(s):
    return s if isinstance(s, bytes) else s.encode("latin-1")

def cstr(s):
    out = '"'
    for c in b(s):
        out += chr(c) if 32 <= c < 127 and chr(c) not in '"\\?' else "\\%03o" % c
    return out + '"'

# ---- -l N and -b SIZE ------------------------------------------------------

SPACES = [" ", "\t", "\n", "\v", "\f", "\r"]
NUMS = ["0", "1", "2", "7", "010", "00", "+3", "+0", " 4", "\t5", "\n6", "\v7", "\f8", "\r9", "  12", "\x805", "", "-1", "-0", " -3", "+", "+-1", "1x", "0x10",
        "1e3", "1.5", "5 ", "1_0", "99999999999999999999", "18446744073709551615", "18446744073709551616", "4294967296", "9223372036854775808", "1844674407370955161", "1844674407370955160", "2000000000000000000", "20000000000000000000", "1000000000000000000", "10000000000000000000"]
SUFFIXES = ["", " ", "b", "B", "k", "K", "m", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q", "c", "w", "g", "t", "kB", "KB", "KiB", "MB", "MiB", "GB", "GiB",
            "TB", "TiB", "PB", "EB", "ZB", "YB", "RB", "QB", "QiB", "QD", "kD", "MD", "iB", "Ki", "KK", "bB", "bD", "bi", "biB", "K ", "KBx", "KiBB", "kb", "KIB",
            "Kib", "Bi", "D", "b ", "bb", "1"]
POW = {"k": 1, "K": 1, "m": 2, "M": 2, "G": 3, "T": 4, "P": 5, "E": 6, "Z": 7, "Y": 8, "R": 9, "Q": 10}

def py_size(s):
    """What split should read s as, by the rule: blanks and a +, the number,
    then b or k..Q with iB/B/D; a suffix with no number is 1 of it. None for
    invalid (an independent restatement)."""
    ws = " \t\n\v\f\r"
    if s and s[0] in "bEGKkMmPQRTYZ":
        num, rest = 1, s
    else:
        t = s.lstrip(ws)
        i = 1 if t[:1] == "+" else 0
        j = i
        while j < len(t) and t[j] in "0123456789":
            j += 1
        if j == i:
            return None
        num, rest = min(int(t[i:j]), MAX64), t[j:]
    if not rest:
        mult = 1
    elif rest == "b":
        mult = 512
    elif rest[0] in POW:
        base, tail = 1024, rest[1:]
        if tail == "iB":
            pass
        elif tail in ("B", "D"):
            base = 1000
        elif tail:
            return None
        mult = base ** POW[rest[0]]
    else:
        return None
    v = min(num * mult, MAX64)
    return v or None

def py_lines(s):
    m = re.fullmatch(r"[ \t\n\v\f\r]*\+?([0-9]+)", s)
    return (min(int(m.group(1)), MAX64) or None) if m else None

def size_cases(opt, unit):
    out = []
    pool = NUMS if unit == "b" else NUMS
    cases = [n + s for n in pool for s in (SUFFIXES if unit == "b" else ["", " ", "k", "K", "b"])]
    if unit == "b":
        cases += [s for s in SUFFIXES]  # bare suffixes
    seen = set()
    for s in cases:
        if s in seen or "\0" in s:
            continue
        seen.add(s)
        p, d, names = run([opt, s], b"")
        ok = p.returncode == 0 and not p.stderr
        if not ok and b"invalid number of" not in p.stderr:
            sys.exit("unexpected for %s %r: %r" % (opt, s, p.stderr))
        cleanup(d, names)
        want = py_size(s) if unit == "b" else py_lines(s)
        if (want is not None) != ok:
            sys.exit("GNU %s %r: %s, the rule says %r" % (opt, s, "accepts" if ok else "rejects", want))
        val = want or 0
        if ok and (val <= 2000000 if unit == "b" else val <= 300):
            # A small one, on a real run: the first piece is that long.
            data = b"x" * (2 * val + 1) if unit == "b" else b"y\n" * (2 * val + 1)
            p2, d2, names2 = run([opt, s], data)
            got = os.path.getsize(os.path.join(d2, "xaa"))
            got = got if unit == "b" else got // 2
            if got != val:
                sys.exit("size of %s %r: GNU piece %d, rule %d" % (opt, s, got, val))
            cleanup(d2, names2)
        out.append((s, int(ok), val))
    return out

# ---- -a LEN ----------------------------------------------------------------

ALENS = ["0", "1", "2", "3", "+1", " 1", "01", "-1", "-0", "-5x", "5x", "", "1k", " -1", "+", "-", "99999999999999999999", "-99999999999999999999", "-9223372036854775807", "-1073741823", "-1073741824", "-1073741825", "-2147483648", "-2147483649", "9223372036854775807",
         "9223372036854775808", "9223372036854775809", "-9223372036854775808", "-9223372036854775809", "2147483647", "2147483648", "4294967295", "4294967296", "18446744073709551615", "18446744073709551616", "2000000000000000000", "20000000000000000000", "-18446744073709551616", "-2000000000000000000", "1844674407370955161",
         "1e1", "0x10", "1 ", " ", "3\n", "\t3", "1.0", "007"]

def alen_cases():
    out = []
    for s in ALENS:
        p, d, names = run(["-a", s], b"")
        e = p.stderr.decode()
        cleanup(d, names)
        if "invalid suffix length" in e:
            if "Numerical result out of range" in e:
                rc, val = -2, 0
            elif "Value too large" in e:
                rc, val = -3, 0
            else:
                rc, val = -1, 0
        elif p.returncode == 0 or "memory exhausted" in e:
            t = s.strip(" \t\n")
            rc, val = 0, min(abs(int(t.lstrip("+") or "0")), (1 << 32) - 1)
        else:
            sys.exit("unexpected for -a %r: %r" % (s, e))
        out.append((s, rc, val))
    return out

# ---- the names ---------------------------------------------------------------

KIND = {"alpha": 0, "num": 1, "hex": 2}

def names_for(kind, alen, frm, nlines):
    args = ["-l", "1", "--verbose"]
    args += {"alpha": [], "num": ["--numeric-suffixes" + ("=" + frm if frm is not None else "")],
             "hex": ["--hex-suffixes" + ("=" + frm if frm is not None else "")]}[kind]
    if alen is not None:
        args += ["-a", str(alen)]
    p, d, names = run(args, b"y\n" * nlines)
    e = p.stderr.decode()
    order = [l[len("creating file '"):-1] for l in p.stdout.decode().split("\n") if l.startswith("creating file '")]
    cleanup(d, names)
    return p.returncode, e, order

CONFIGS = [
    # kind, -a, FROM, lines of input
    ("alpha", None, None, 17900), ("alpha", 0, None, 700), ("alpha", 1, None, 30), ("alpha", 2, None, 700), ("alpha", 3, None, 17600), ("alpha", 4, None, 60),
    ("num", None, None, 1100), ("num", 0, None, 120), ("num", 1, None, 15), ("num", 2, None, 120), ("num", 3, None, 1100), ("num", 4, None, 30),
    ("hex", None, None, 4300), ("hex", 0, None, 300), ("hex", 1, None, 20), ("hex", 2, None, 300), ("hex", 3, None, 4200), ("hex", 4, None, 30),
    ("num", None, "0", 120), ("num", None, "5", 120), ("num", None, "05", 120), ("num", None, "007", 120), ("num", None, "85", 40), ("num", None, "99", 5),
    ("num", 0, "5", 120), ("num", 0, "95", 20), ("num", 1, "5", 20), ("num", 3, "5", 1100), ("num", 3, "999", 5), ("num", 3, "0098", 5), ("num", 5, "12345", 30),
    ("num", 2, "1", 120), ("num", 4, "000", 30),
    ("num", 2, "000", 30), ("num", 2, "0000", 30), ("num", 1, "00", 5), ("num", 1, "0", 5), ("num", 2, "0100", 5), ("num", 3, "00099", 5), ("hex", 2, "000", 30), ("hex", 1, "00", 20),
    ("hex", 2, "0100", 5), ("num", 2, "00", 120),
    ("hex", None, "0", 300), ("hex", None, "a", 300), ("hex", None, "ff", 5), ("hex", None, "fe", 5), ("hex", None, "0fe", 5), ("hex", 0, "ab", 300), ("hex", 3, "0fe", 20),
    ("hex", 3, "fff", 5), ("hex", 1, "e", 5), ("hex", 4, "00ff", 300),
    # a start that does not fit, or is no number
    ("num", None, "100", 5), ("num", None, "99999999999999999999", 5), ("num", 3, "1000", 5), ("num", 1, "10", 5), ("hex", None, "100", 5), ("hex", 2, "fff", 5),
    ("hex", 1, "10", 5), ("num", None, "x", 5), ("num", None, "+5", 5), ("num", None, " 5", 5), ("num", None, "5 ", 5), ("num", None, "0x5", 5), ("num", None, "-1", 5),
    ("num", None, "1e2", 5), ("num", None, "5a", 5), ("num", None, "ff", 5), ("hex", None, "F", 5), ("hex", None, "g", 5), ("hex", None, "0F", 5), ("hex", None, "0x5", 5),
    ("hex", None, "+5", 5), ("hex", None, "1g", 5),
]

def sample_indexes(order):
    n = len(order)
    keep = set(range(min(n, 300)))
    keep |= set(range(0, n, 97))
    for i in range(1, n):
        if len(order[i]) != len(order[i - 1]):
            keep |= set(range(max(0, i - 3), min(n, i + 4)))
    keep |= set(range(max(0, n - 4), n))
    return sorted(keep)

def suffix_cases():
    out = []
    for kind, alen, frm, nlines in CONFIGS:
        rc, err, order = names_for(kind, alen, frm, nlines)
        status = 0  # 0 = names, 1 = bad start, 2 = start too large
        if "invalid start value" in err:
            status = 1
        elif "too large for the suffix length" in err:
            status = 2
        elif rc != 0 and "exhausted" not in err:
            sys.exit("unexpected for %r: %r" % ((kind, alen, frm), err))
        if status:
            out.append((kind, alen or 0, frm, status, 0, 0, []))
            continue
        if any(not n.startswith("x") for n in order):
            sys.exit("name without the x")
        sufs = [n[1:] for n in order]
        if kind == "hex" and frm is not None and re.search("[a-f]", frm):
            # GNU 9.4 goes wrong from a start with a letter in it: it reads the
            # alphabet past its end (0a is followed by 0e, 0c by 00, ...). Only
            # the first name is right.
            out.append((kind, alen or 0, frm, 0, 1, -1, [(0, sufs[0])]))
            continue
        samples = [(i, sufs[i]) for i in sample_indexes(sufs)]
        out.append((kind, alen or 0, frm, 0, len(sufs), int("exhausted" in err), samples))
    return out

def main():
    lines, bytes_, alens, sufs = size_cases("-l", "l"), size_cases("-b", "b"), alen_cases(), suffix_cases()
    w = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "spcore_cases.h"), "w")
    w.write("// Made by gen_spcore_cases.py from GNU split's answers. Do not edit.\n")
    w.write("typedef struct { const char* s; int ok; uint64_t val; } sp_size_case_t;\n")
    w.write("typedef struct { const char* s; int rc; uint32_t val; } sp_alen_case_t;\n")
    w.write("typedef struct { unsigned idx; const char* suffix; } sp_sample_t;\n")
    w.write("// status: 0 names, 1 FROM is no number, 2 FROM does not fit; total names made; exhausted: the run ended with no more names (-1: not checked).\n")
    w.write("typedef struct { int kind; unsigned alen; const char* from; int status; unsigned total; int exhausted; unsigned nsamples; const sp_sample_t* samples; } sp_suffix_case_t;\n\n")
    for name, cases in (("sp_lines_cases", lines), ("sp_bytes_cases", bytes_)):
        w.write("static const sp_size_case_t %s[] = {\n" % name)
        for s, ok, val in cases:
            w.write("    {%s, %d, %dull},\n" % (cstr(s), ok, val))
        w.write("};\n\n")
    w.write("static const sp_alen_case_t sp_alen_cases[] = {\n")
    for s, rc, val in alens:
        w.write("    {%s, %d, %du},\n" % (cstr(s), rc, val))
    w.write("};\n\n")
    for i, (kind, alen, frm, status, total, exh, samples) in enumerate(sufs):
        if samples:
            w.write("static const sp_sample_t sp_samples_%d[] = {" % i)
            w.write(", ".join('{%d, "%s"}' % (j, s) for j, s in samples))
            w.write("};\n")
    w.write("\nstatic const sp_suffix_case_t sp_suffix_cases[] = {\n")
    for i, (kind, alen, frm, status, total, exh, samples) in enumerate(sufs):
        w.write("    {%d, %d, %s, %d, %d, %d, %d, %s},\n" % (KIND[kind], alen, cstr(frm) if frm is not None else "0", status, total, exh,
                                                              len(samples), "sp_samples_%d" % i if samples else "0"))
    w.write("};\n")
    w.close()
    print("lines %d, bytes %d, -a %d, suffix configs %d" % (len(lines), len(bytes_), len(alens), len(sufs)))

main()

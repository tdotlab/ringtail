#!/usr/bin/env python3
"""Writes pfcore_cases.h: a format, its arguments, and what GNU printf wrote
(standard output and standard error, in the order they were written) and
its exit status. Needs GNU printf (/usr/bin/printf). The generated file is
checked in, so test_pfcore needs nothing but a compiler; rerun this only to
change the cases.

GNU printf is run as "printf" (so its messages read as ours do) and with the
C locale. The conversions that are not done here (%a %e %f %g and %q) are not
in the cases."""
import os, random, subprocess, sys

random.seed(2718)
GNU = "/usr/bin/printf"

def run(fmt, args):
    p = subprocess.run([b"printf", fmt, *args], executable=GNU, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"}, timeout=60)
    return p.stdout, p.returncode

def b(s):
    return s if isinstance(s, bytes) else s.encode("latin-1")

NUMS = ["0", "1", "-1", "7", "42", "255", "256", "65535", "-255", "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296",
        "9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809", "18446744073709551615", "18446744073709551616",
        "99999999999999999999", "-99999999999999999999", "0x1f", "0X1F", "0xff", "0x7fffffffffffffff", "0x8000000000000000", "-0x8000000000000000",
        "0xffffffffffffffff", "0x10000000000000000", "-0x1", "010", "0777", "08", "09", "00", "0b101", "0B11", "0b", "0b2", "0x", "0xg", "0X", "1e3",
        "1.5", ".5", "5.", "+5", "+", "-", "--5", "+-5", " 12", "12 ", "\t3", "3\n", "\n3", " ", "", "abc", "5x", "5 x", "0x1p3", "1_000", "1,5",
        "'a", '"a', "'ab", "'abc", '"ab', "'", '"', "''", "'\xe9", "'\xc3\xa9", "'0", "' ", "'\\", "'\\n", "\x01", "5\x01", "a\x7f", "x\\y", "it's", "tab\there",
        "nl\nhere", "esc\x1bx", "high\xe9\xff", "0x8", "-0", "+0", "-0x0", "1" * 19, "1" * 20, "7" * 22, "0" * 30 + "5", "-" + "9" * 19]

GOOD = ["0", "1", "-1", "7", "42", "255", "256", "65535", "-255", "2147483647", "2147483648", "-2147483648", "4294967296", "9223372036854775807",
        "-9223372036854775808", "18446744073709551615", "0x1f", "0X1F", "0xff", "0x7fffffffffffffff", "0xffffffffffffffff", "-0x1", "010", "0777", "0b101", "+5",
        " 12", "\t3", "'a", '"a', "'ab", "'\xe9", "-0", "+0", "00", "0" * 30 + "5"]

def number():
    r = random.random()
    if r < 0.5:
        return random.choice(GOOD)
    if r < 0.6:
        return random.choice(NUMS)
    if r < 0.9:
        return str(random.randrange(-300, 300))
    if r < 0.95:
        return random.choice(["0x", "0", "", "-", "+"]) + "".join(random.choice("0123456789abcdefABCDEFxg ") for _ in range(random.randrange(0, 20)))
    return "".join(chr(random.randrange(1, 256)) for _ in range(random.randrange(1, 6)))

def text():
    r = random.random()
    if r < 0.5:
        return "".join(random.choice("abc xyz,.;:!-_/0189\t") for _ in range(random.randrange(0, 9)))
    if r < 0.8:
        return random.choice(["", "hello", "a", "é", "\xc3\xa9", "tab\tx", "back\\slash", "%s", "%d", "100%", "it's", "quote\"", "a\nb", "\\n", "\\x41", "x" * 30, "  pad"])
    return "".join(chr(random.randrange(1, 256)) for _ in range(random.randrange(1, 6)))

ESCAPES = ["\\n", "\\t", "\\r", "\\a", "\\b", "\\e", "\\f", "\\v", "\\\\", '\\"', "\\'", "\\q", "\\c", "\\0", "\\00", "\\000", "\\0101", "\\1", "\\12", "\\101", "\\1011",
           "\\777", "\\400", "\\8", "\\x41", "\\x4", "\\x414", "\\xff", "\\xg", "\\x", "\\xfg", "\\u0041", "\\u0000", "\\u007f", "\\u0080", "\\u00e9", "\\u00E9", "\\u1234",
           "\\ud7ff", "\\ud800", "\\udfff", "\\U00000041", "\\U00000080", "\\U0010ffff", "\\U00110000", "\\U0000d800", "\\u41", "\\u", "\\U1234", "\\ug", "\\", "\\x4g"]

def esc():
    return random.choice(ESCAPES[:30] if random.random() < 0.6 else ESCAPES)

FLAGS = "-+ #0'I"

def directive():
    r = random.random()
    if r < 0.05:
        return random.choice(["%b", "%%", "%b", "%%"])
    flags = "".join(random.choice(FLAGS) for _ in range(random.choice([0, 0, 0, 1, 1, 2, 3])))
    w = random.choice(["", "", "", "1", "3", "5", "8", "12", "20", "*", "*", "05"])
    pr = random.choice(["", "", "", ".", ".0", ".1", ".3", ".6", ".12", ".*", ".*"])
    mod = random.choice(["", "", "", "", "", "l", "ll", "h", "hh", "j", "z", "t", "L", "lh"])
    conv = random.choice(list("dddiiuuooxxXXcccsssss") * 3 + ["b", "%", "n", "p", "y", "D", "U", "C", "S", "k", " ", "5", "$", "", "m"])
    return "%" + flags + w + pr + mod + conv

def format_():
    parts = []
    for _ in range(random.choice([1, 1, 2, 2, 3, 4])):
        r = random.random()
        parts.append(directive() if r < 0.55 else esc() if r < 0.7 else text())
    return "".join(parts)

def has_float(fmt):
    """Whether GNU would take fmt for a conversion that is not done here (%a %e %f %g, %q): the format is read the way GNU reads it."""
    i, n = 0, len(fmt)
    while i < n:
        c = fmt[i]
        if c == "\\":
            i += 2
            continue
        if c != "%":
            i += 1
            continue
        i += 1
        if i < n and fmt[i] in "%b":
            i += 1
            continue
        if i < n and fmt[i] == "q":
            return True
        while i < n and fmt[i] in FLAGS:
            i += 1
        if i < n and fmt[i] == "*":
            i += 1
        else:
            while i < n and fmt[i].isdigit():
                i += 1
        if i < n and fmt[i] == ".":
            i += 1
            if i < n and fmt[i] == "*":
                i += 1
            else:
                while i < n and fmt[i].isdigit():
                    i += 1
        while i < n and fmt[i] in "lLhjtz":
            i += 1
        if i < n and fmt[i] in "aAeEfFgG":
            return True
        i += 1
    return False

def args_for(fmt):
    nd = fmt.count('%') + fmt.count('*')
    n = random.choice([nd, nd, nd, nd, nd + 1, nd * 2, max(nd - 1, 0), 0])
    out = []
    for _ in range(n):
        out.append(number() if random.random() < 0.7 else text() if random.random() < 0.8 else "".join(random.choice("\\abcnx01234567cuU") for _ in range(random.randrange(1, 8))))
    return out

EDGES = [
    ("", []), ("", ["x"]), ("x", []), ("x", ["a", "b"]), ("%s", []), ("%d", []), ("%c", []), ("%b", []), ("%s%s", ["a"]), ("%d %d\n", ["1", "2", "3"]),
    ("%s\n", ["a", "b", "c"]), ("%%", []), ("%%\n", ["x"]), ("%", []), ("%5", []), ("%-", []), ("%.", []), ("%l", []), ("%ll d", []), ("%1$s", ["a"]),
    ("%5%", []), ("%-%", []), ("%0%", []), ("a%", ["x"]), ("%c|%c", ["hello", ""]), ("%.0s|%.s|", ["abc", "abc"]), ("%.d|%.0d|", ["0", "0"]),
    ("%+d|% d|%+d|% d", ["5", "5", "-5", "-5"]), ("%#o|%#x|%#X|%#o", ["8", "255", "255", "0"]), ("%.0x|%#.0x|%#.0o|", ["0", "0", "0"]),
    ("%*d|%-*d|%.*d", ["5", "1", "5", "1", "3", "1"]), ("%*d", ["-5", "1"]), ("%.*d", ["-1", "5"]), ("%*d", []), ("%*.*d", ["3"]), ("%-*d|", ["3"]),
    ("%*d", ["x", "1"]), ("%*d", ["2147483648", "1"]), ("%.*d", ["2147483648", "1"]), ("%*d", ["-2147483649", "1"]), ("%*d|", ["2147483647", "1"]),
    ("%*d|", ["-2147483648", "1"]), ("%.*s|", ["2147483647", "a"]), ("%*s|", ["2147483647", "a"]), ("%*c|", ["2147483647", "a"]),
    ("%2147483647d|", ["1"]), ("%2147483648d|", ["1"]), ("%99999999999d|", ["1"]), ("%.2147483648d|", ["1"]), ("%.2147483648s|", ["a"]),
    ("%.2147483647s|", ["a"]), ("%2147483647s|", ["a"]), ("%2147483647c|", ["a"]), ("%.99999d", ["1"]),
    ("%b", ["a\\nb"]), ("%b", ["a\\0101b"]), ("%b", ["a\\101b"]), ("%b", ["a\\cb"]), ("%b|%s", ["x\\cy", "z"]), ("%b", ["\\x41\\x4g"]), ("%b", ["\\q\\\\"]),
    ("%b", ["\\0"]), ("%b", ["\\00001"]), ("%b", ["a\\"]), ("%b", ["\\x"]), ("%b", ["\\u0041\\u00e9"]), ("%b", ["\\ud800"]), ("%b", ["\\U00000080"]),
    ("%b%b", ["a\\0", "b"]), ("%5b|", ["ab"]), ("%.1b|", ["abc"]), ("%-5b|", ["ab"]), ("%b\n", []),
    ("%d\\c|", ["abc"]), ("%d|%b|%d", ["abc", "x\\cy", "5"]), ("a\\cb", []), ("%s|%d", ["a\\cb"]), ("%d%d\\c", ["x", "1", "2"]),
    ("%d", ["5x"]), ("%d", ["abc"]), ("%d", [""]), ("%d", ["'a"]), ("%d", ["'ab"]), ("%d", ["'"]), ("%d", ["99999999999999999999", "1"]),
    ("%u", ["-1"]), ("%u", ["99999999999999999999"]), ("%d", ["-9223372036854775809"]), ("%x", ["-1"]), ("%o", ["-1"]),
    ("%05d|%-05d|%05s|%05.1d", ["42", "42", "ab", "7"]), ("%08.3d|%+08d|% 08d|%-+8d|", ["5", "5", "5", "5"]), ("%#5.3x|%-#8x|%08x|%#08x", ["10", "255", "255", "255"]),
    ("%5.0d|%+5.0d|", ["0", "0"]), ("%--5d|", ["1"]), ("%-+ #0x|", ["1"]), ("%+u|%+x|", ["5", "5"]), ("%hhd %ld %lld %jd %zd %td %Ld", ["1", "2", "3", "4", "5", "6", "7"]),
    ("%5c|%-5c|", ["a", "b"]), ("%c%c%c", ["a", "bcd", ""]), ("%s", ["é"]), ("%.1s|", ["é"]), ("%3s|", ["é"]), ("%.3c|", ["abc"]), ("%05c|", ["a"]),
    ("\\xg", []), ("\\x41\\x4", []), ("\\101\\1011\\0101", []), ("\\u0041", []), ("\\u00e9", []), ("\\U00000041x", []), ("\\u41", []), ("\\ud800", []),
    ("\\a\\b\\e\\f\\n\\r\\t\\v\\\\\\\"|", []), ("\\q", []), ("\\", []), ("a\\", []), ("\\0101\\00\\08", []), ("\\8", []), ("\\777", []), ("\\400", []),
    ("%y", []), ("%z", []), ("%lz", []), ("%5.", ["x"]), ("%.5", ["x"]), ("%*", ["3", "x"]), ("%.0*d|", ["5", "1"]), ("%0*d|", ["5", "1"]),
    ("%d%%%s\n", ["5", "x", "6", "y"]), ("[%s]\n", ["a b", "c"]), ("%s %s\n", ["a"]), ("%s", ["a", "b", "c"]), ("x%sy%sz\n", ["1", "2", "3", "4", "5"]),
    ("%d %s\n", ["1", "one", "2", "two", "3"]), ("%5s|%-5s|%.2s|%5.2s|", ["abc", "abc", "abc", "abc"]), ("%x|%X|%o|%u", ["255", "255", "8", "7"]),
    ("%d", ["\xc3\xa9"]), ("%d", ["a\nb"]), ("%d", ["a\tb"]), ("%d", ["a\\b"]), ("%d", ["a'b"]), ("%d", ["a\x01"]), ("%d", ["5\x7f"]), ("%d", ["5\\"]),
    ("%d", ["'\xe9"]), ("%d", ["'\xc3\xa9"]), ("%d", ["\"a"]),
    ("%s%s", ["a", "b", "c"]), ("", ["a", "b"]), ("\\n", ["a"]), ("%b", ["a", "b"]),
    # leading white space of every kind strtoimax skips, and the hex digit 0
    ("%d|%d|%d|%d|%d|%d|%d", ["\t5", "\n5", "\v5", "\f5", "\r5", "\x085", "\x0e5"]), ("%d|%d", ["5\r", "\r"]),
    ("\\x0|\\x00|\\x07x|\\x0g|\\x9|\\xa|\\xf|\\xF|\\x1G", []), ("%b", ["\\x0|\\x00|\\x07x|\\x0g|\\x9|\\xa|\\xf|\\xF|\\x1G"]),
    # an INT_MIN width from a * can not be written, unless nothing is
    ("%*c|", ["-2147483648", "a"]), ("%*s|%*s|%*.0s|%*.1s|", ["-2147483648", "a", "-2147483648", "", "-2147483648", "abc", "-2147483648", "abc"]),
    ("%-*s|", ["-2147483648", "ab"]), ("%*s|", ["-2147483648", ""]), ("%*c|", ["-2147483648", ""]), ("%*.0d|%*.0x|%*.1d|", ["-2147483648", "0", "-2147483648", "0", "-2147483648", "0"]),
    ("\\u0000|\\u0009|\\u000f|\\u000F|\\u00a0", []), ("\\U0000000a|\\U00000000", []),
]

N = int(sys.argv[1]) if len(sys.argv) > 1 else 4500
cases = []
seen = set()
for fmt, args in EDGES:
    cases.append((fmt, args))
while len(cases) < N + len(EDGES):
    fmt = format_()
    if has_float(fmt):
        continue
    args = args_for(fmt)
    key = (fmt, tuple(args))
    if key in seen:
        continue
    seen.add(key)
    cases.append((fmt, args))

def c_str(s):
    out = []
    for x in s:
        if x == 0x22: out.append('\\"')
        elif x == 0x5c: out.append("\\\\")
        elif x == 0x3f: out.append("\\?")  # no trigraphs
        elif 0x20 <= x < 0x7f: out.append(chr(x))
        else: out.append("\\%03o" % x)
    return '"' + "".join(out) + '"'

kinds = {"ok": 0, "msg": 0}
rows = []
for t, (fmt, args) in enumerate(cases):
    f, a = b(fmt), [b(x) for x in args]
    assert b"\0" not in f and all(b"\0" not in x for x in a)
    out, st = run(f, a)
    if t >= len(EDGES) and len(out) > 2000:
        continue  # a width taken from an argument can be anything: keep the file small
    rows.append((f, a, out, st))
    kinds["msg" if b"printf:" in out else "ok"] += 1

MAXARGS = max(len(r[1]) for r in rows)
with open("tests/pfcore_cases.h", "w") as f:
    f.write("// Made by tests/gen_pfcore_cases.py: a format, its arguments, and what GNU printf wrote (standard output and\n")
    f.write("// standard error, in the order they were written) and its exit status.\n")
    f.write("typedef struct { const char* format; int nargs; const char* args[%d]; const char* want; unsigned wantlen; int status; } pf_case_t;\n" % MAXARGS)
    f.write("static const pf_case_t pf_cases[] = {\n")
    for fmt, args, out, st in rows:
        al = ", ".join(c_str(x) for x in args) or "0"
        f.write("    { %s, %d, { %s }, %s, %d, %d },\n" % (c_str(fmt), len(args), al, c_str(out), len(out), st))
    f.write("};\n")
print(len(rows), "cases:", kinds)

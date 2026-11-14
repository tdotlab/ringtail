#!/usr/bin/env python3
"""Writes jncore_cases.h: what GNU join made of field numbers, of the items of
an -o list, of lines cut into fields, and of pairs of keys. Needs GNU join
(/usr/bin/join). The generated file is checked in, so test_jncore needs
nothing but a compiler; rerun this only to change the cases.

How the answers are found, all by running join in the C locale:
 - A field number N is tried as "-1 N -j 1" on two empty files: an error
   message says it is no good, and "incompatible join fields A, 0" says that
   N counted from 0 is A (no message: 0).
 - The N of -a N is tried by whether "-a N" writes the unpaired line of file 1.
 - An item of -o is tried on two one-line files of 61 fields (a1 .. a60 and a
   key; b1 .. b60 and the key); the field it writes says which it is.
 - A line is cut into fields by writing it as an unpaired line with a join
   field far past its end: the output is a filler, and then each field after a
   separator (a filler for an empty one), so the number of fields and each
   of them can be read. All the lines of one separator go in one run.
 - Two keys are compared by joining two one-line files whole (-t ''): one line
   written means equal, else the smaller key is written first."""
import os, random, subprocess, sys, tempfile

GNU = "/usr/bin/join"
ENV = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
FILL = b"\x01"

def run(args, files):
    d = tempfile.mkdtemp(prefix="jngen")
    names = []
    for i, data in enumerate(files):
        n = os.path.join(d, "f%d" % i)
        with open(n, "wb") as f:
            f.write(data)
        names.append(n)
    p = subprocess.run([b"join", *args, *[n.encode() for n in names]], executable=GNU, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=ENV, timeout=60)
    for n in names:
        os.unlink(n)
    os.rmdir(d)
    return p

def b(s):
    return s if isinstance(s, bytes) else s.encode("latin-1")

def cstr(s):
    out = '"'
    for c in b(s):
        out += chr(c) if 32 <= c < 127 and chr(c) not in '"\\?' else "\\%03o" % c
    return out + '"'

# ---- field numbers -----------------------------------------------------------

FIELD_STRS = ["1", "2", "3", "7", "10", "40", "41", "0", "-1", "+1", "+0", "-0", " 1", "\t2", "\n3", "\v4", "\f5", "\r6", "  7", "1 ", " 1 ", "", "+", "-",
              "1x", "x1", "0x10", "010", "00", "1e3", "1.0", "1,2", "99999999999999999999", "9223372036854775807", "9223372036854775808",
              "9223372036854775806", "9223372036854775805", "18446744073709551616", "-9223372036854775808", "-9223372036854775809", "-99999999999999999999", "4294967295",
              "4294967296", "2147483647", "2147483648", "1_0", "+ 1", "- 1", "++1", "--1", "+-1", "0 ", "99999999999999999999x", "99999999999999999999 ",
              "00000000000000000000000000000001", "000000000000000000000000000000099999999999999999999", "\xa01", "\x851", "1\x0b", "1\n", "\n\n1", " +5", "+ 5",
              "9223372036854775807x", "0000009223372036854775807", "9223372036854775808x"]

def field_case(s):
    p = run([b"-1", b(s), b"-j", b"1"], [b"", b""])
    err = p.stderr
    if b"invalid field number" in err:
        return (s, 0, 0)
    if b"incompatible join fields" in err:
        a = err.split(b"fields ")[1].split(b",")[0]
        return (s, 1, int(a))
    if p.returncode == 0:
        return (s, 1, 0)
    sys.exit("field %r: unexpected: %r %r" % (s, p.returncode, err))

# ---- the N of -a N -----------------------------------------------------------

FILENUM_STRS = ["1", "2", "3", "0", "-1", "+1", "+2", " 1", "1 ", "01", "002", "2x", "", "x", "1.0", "99999999999999999999", "9223372036854775809", "-2",
                "\t2", "\n1", "+", " ", "1\n", "0x1", "1e0", "4294967297", "18446744073709551617", "-18446744073709551615"]

def filenum_case(s):
    p = run([b"-a", b(s)], [b"x\n", b""])
    if b"invalid field number" in p.stderr:
        return (s, 0)
    if p.returncode == 0 and p.stdout == b"x\n":
        return (s, 1)
    if p.returncode == 0 and p.stdout == b"":
        return (s, 2)
    sys.exit("filenum %r: unexpected: %r %r" % (s, p.stdout, p.stderr))

# ---- the items of -o ---------------------------------------------------------
# One item each: -o cuts its list at commas, blanks and tabs before it reads any.

SPEC_ITEMS = ["0", "1.1", "1.2", "2.7", "2.60", "1.41", "2.59", "1.61", "0.1", "0.", "00", "1", "2", "1.", "2.", "1.0", "1.-1", "1.+3", "2.007", "1.x", "3.1", "3", "a",
              "x.1", "", "1:1", "1.1.1", "1.99999999999999999999", "2.9223372036854775807", "1.9223372036854775808", "1.\x0b3", "2.\n4", "1.3x", "9.1",
              "01.1", "1.01", "1.+0", "-1.1", "+1.1", "1.1\x01", "0\x01", "1.60", "2.1"]

A = b" ".join(b"a%d" % i for i in range(1, 61)) + b" key\n"
B = b" ".join(b"b%d" % i for i in range(1, 61)) + b" key\n"

def spec_case(s):
    p = run([b"-j", b"61", b"-o", b(s)], [A, B])
    err = p.stderr
    if b"invalid field specifier" in err:
        return (s, 1, 0, 0)
    if b"invalid file number in field spec" in err:
        return (s, 2, 0, 0)
    if b"invalid field number" in err:
        return (s, 3, 0, 0)
    if p.returncode != 0:
        sys.exit("spec %r: unexpected: %r %r" % (s, p.returncode, err))
    o = p.stdout.rstrip(b"\n")
    if s == "0":
        return (s, 0, 0, 0)
    f = int(s[0])
    if o[:1] in (b"a", b"b") and o[1:].isdigit():
        assert (o[:1] == b"a") == (f == 1), (s, o)
        return (s, 0, f, int(o[1:]) - 1)
    return (s, 0, f, -1)  # past the end (or the key): not seen in the output

# ---- cutting lines into fields -----------------------------------------------

def cut_lines(tab, lines, zero):
    """tab: None (blanks), an int (that byte), or "\\n" for -t ''."""
    args = [b"--nocheck-order", b"-a", b"1", b"-j", b"50", b"-e", FILL]
    if tab is None:
        sep = b" "
    elif tab == "\n":
        args += [b"-t", b""]
        sep = b"\n"
    elif tab == 0:
        args += [b"-t", b"\\0"]
        sep = b"\0"
    else:
        args += [b"-t", bytes([tab])]
        sep = bytes([tab])
    eol = b"\0" if zero else b"\n"
    if zero:
        args.append(b"-z")
    data = b"".join(l + eol for l in lines)
    p = run(args, [data, b""])
    if p.returncode != 0:
        sys.exit("cut %r: %r %r" % (tab, p.returncode, p.stderr))
    recs = p.stdout.split(eol)
    assert recs[-1] == b"", recs[-3:]
    recs = recs[:-1]
    assert len(recs) == len(lines), (len(recs), len(lines))
    res = []
    for l, r in zip(lines, recs):
        parts = r.split(sep, 1) if tab == "\n" else r.split(sep)
        assert parts[0] == FILL, (l, r)
        res.append((l, [b"" if x == FILL else x for x in parts[1:]]))
    return res

def random_lines(rng, alpha, n, maxlen):
    out = set()
    while len(out) < n:
        out.add(b"".join(rng.choice(alpha) for _ in range(rng.randint(0, maxlen))))
    return sorted(out)

def split_cases():
    rng = random.Random(7)
    cases = []
    fixed_blank = [b"", b" ", b"  ", b"\t", b" \t ", b"a", b" a", b"a ", b" a ", b"a b", b"a  b", b"a b ", b"a\tb", b"a \t b", b"  a  b  ", b"a b c d e f g h",
                   b"\ta\t", b"a  ", b"a\t\t", b"x y  z   w", b"  \t  x", b"abc", b"ab cd  ef \t gh\t"]
    alpha = [b"a", b"b", b" ", b"\t", b"  ", b"xy", b"a", b" "]
    lines = fixed_blank + [l for l in random_lines(rng, alpha, 400, 14) if l not in fixed_blank]
    for l, f in cut_lines(None, lines, False):
        cases.append((-1, l, f))
    # With -z a line may hold newlines, which separate fields as blanks do.
    nl = [b"a\nb", b"\n", b"a\n", b"\na", b"a \n b", b" \n ", b"x\n\ny", b"\n\n", b"a b\nc d"]
    alpha = [b"a", b"b", b" ", b"\t", b"\n", b"xy"]
    lines = nl + [l for l in random_lines(rng, alpha, 150, 10) if l not in nl]
    for l, f in cut_lines(None, lines, True):
        cases.append((-1, l, f))
    for tab, alpha in ((ord(","), [b"a", b",", b"b", b",,", b" ", b"xy"]), (ord(" "), [b"a", b" ", b"b", b"  ", b"\t", b"xy"]), (ord("\t"), [b"a", b"\t", b"b", b" ", b"xy"]),
                       (ord("a"), [b"a", b"b", b"aa", b" ", b"c"]), (0, [b"a", b"\0", b"b", b"\0\0", b" "]), (0xe9, [b"a", b"\xe9", b"b", b" "])):
        fixed = [b"", bytes([tab]), bytes([tab]) * 2, b"a", b"a" + bytes([tab]), bytes([tab]) + b"a", b"a" + bytes([tab]) + b"b", b"a" + bytes([tab]) * 2 + b"b"]
        lines = fixed + [l for l in random_lines(rng, alpha, 120, 10) if l not in fixed]
        for l, f in cut_lines(tab, lines, False):
            cases.append((tab, l, f))
    # With -z, a line may hold newlines: with -t '' they stay inside the one field.
    lines = [b"", b"a", b" ", b"a b", b"a,b", b"\t", b"  x  ", b"a" * 300, b"a\nb", b"\n", b"a\n", b"\na", b"a\n\nb", b"a b\nc d"]
    for l, f in cut_lines("\n", lines, True):
        cases.append((ord("\n"), l, f))
    return cases

# ---- comparing keys ----------------------------------------------------------

def key_pairs():
    rng = random.Random(11)
    fixed = [(b"a", b"A"), (b"A", b"a"), (b"a", b"B"), (b"A", b"b"), (b"Z", b"a"), (b"z", b"A"), (b"_", b"a"), (b"_", b"A"), (b"[", b"a"), (b"`", b"A"), (b"{", b"Z"),
             (b"@", b"a"), (b"", b""), (b"", b"a"), (b"a", b""), (b"ab", b"a"), (b"a", b"ab"), (b"ab", b"AB"), (b"aB", b"Ab"), (b"abc", b"ABD"), (b"abc", b"abd"),
             (b"\xe9", b"\xc9"), (b"\xff", b"a"), (b"a", b"\x80"), (b"a b", b"a"), (b"a\tb", b"a b"), (b"\0", b""), (b"\0", b"\0"), (b"a\0b", b"a\0c"), (b"a\0", b"a"),
             (b"Hello", b"hello"), (b"hello", b"help"), (b"x", b"x"), (b"X", b"x")]
    alpha = [b"a", b"A", b"b", b"B", b"z", b"Z", b"_", b"[", b"`", b"{", b"@", b"0", b"\x7f", b"\x80", b"\xc3", b"\xe3", b"\xff", b" ", b"\t"]
    out = list(fixed)
    while len(out) < 400:
        out.append((b"".join(rng.choice(alpha) for _ in range(rng.randint(0, 4))), b"".join(rng.choice(alpha) for _ in range(rng.randint(0, 4)))))
    return out

def key_case(x, y, icase):
    args = [b"-t", b"", b"-a", b"1", b"-a", b"2", b"--nocheck-order"] + ([b"-i"] if icase else [])
    p = run(args, [x + b"\n", y + b"\n"])
    if p.returncode != 0:
        sys.exit("keys %r %r: %r %r" % (x, y, p.returncode, p.stderr))
    lines = p.stdout.split(b"\n")[:-1]
    if len(lines) == 1:
        return 0
    assert len(lines) == 2, (x, y, p.stdout)
    assert x != y
    return -1 if lines[0] == x else 1

def main():
    fields = [field_case(s) for s in FIELD_STRS]
    nums = [filenum_case(s) for s in FILENUM_STRS]
    specs = [spec_case(s) for s in SPEC_ITEMS]
    cuts = split_cases()
    keys = []
    for x, y in key_pairs():
        for ic in (0, 1):
            keys.append((x, y, ic, key_case(x, y, ic)))
    w = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "jncore_cases.h"), "w")
    w.write("// Made by gen_jncore_cases.py from GNU join's answers. Do not edit.\n")
    w.write("typedef struct { const char* s; int ok; unsigned long long val; } jn_field_case_t;\n")
    w.write("typedef struct { const char* s; int which; } jn_filenum_case_t;\n")
    w.write("// status: 0 good, 1 invalid field specifier, 2 invalid file number, 3 invalid field number; field -1: past the end, not seen.\n")
    w.write("typedef struct { const char* s; int status, file; int field; } jn_spec_case_t;\n")
    w.write("typedef struct { const char* s; unsigned n; } jn_str_t;\n")
    w.write("typedef struct { int tab; jn_str_t line; unsigned nf; const jn_str_t* f; } jn_cut_case_t;\n")
    w.write("typedef struct { jn_str_t a, b; int icase, order; } jn_key_case_t;\n\n")
    w.write("static const jn_field_case_t jn_field_cases[] = {\n")
    for s, ok, v in fields:
        w.write("    {%s, %d, %dull},\n" % (cstr(s), ok, v))
    w.write("};\n\nstatic const jn_filenum_case_t jn_filenum_cases[] = {\n")
    for s, v in nums:
        w.write("    {%s, %d},\n" % (cstr(s), v))
    w.write("};\n\nstatic const jn_spec_case_t jn_spec_cases[] = {\n")
    for s, st, f, fld in specs:
        w.write("    {%s, %d, %d, %d},\n" % (cstr(s), st, f, fld))
    w.write("};\n\n")
    for i, (tab, l, f) in enumerate(cuts):
        if f:
            w.write("static const jn_str_t jn_cut_%d[] = {%s};\n" % (i, ", ".join("{%s, %d}" % (cstr(x), len(x)) for x in f)))
    w.write("\nstatic const jn_cut_case_t jn_cut_cases[] = {\n")
    for i, (tab, l, f) in enumerate(cuts):
        w.write("    {%d, {%s, %d}, %d, %s},\n" % (tab, cstr(l), len(l), len(f), "jn_cut_%d" % i if f else "0"))
    w.write("};\n\nstatic const jn_key_case_t jn_key_cases[] = {\n")
    for x, y, ic, o in keys:
        w.write("    {{%s, %d}, {%s, %d}, %d, %d},\n" % (cstr(x), len(x), cstr(y), len(y), ic, o))
    w.write("};\n")
    w.close()
    print("field %d, filenum %d, spec %d, cut %d, key %d" % (len(fields), len(nums), len(specs), len(cuts), len(keys)))

main()

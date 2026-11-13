#!/usr/bin/env python3
"""Writes b64_cases.h: inputs for base64 and what GNU base64 writes for
them, with its exit status. Needs GNU base64. The generated file is checked
in, so test_b64 needs nothing but a compiler; rerun this only to change the
cases.

Three kinds: encoding with a given -w argument (the argument is a string,
so the strange ones, " 3", "-0", "5x", a number too big for 32 bits, are
there to see what GNU makes of them), decoding, and decoding with -i. The
decoding inputs are mostly good text with a few characters changed, since
what matters is where GNU stops and what it has written by then."""
import random, subprocess

random.seed(4648)
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
POOL = list(ALPHA) + ["=", "=", "=", "\n", "\n", "!", " ", "\t", "\r", "\xff", "-", "_", "\x00"]

def gnu(args, data):
    p = subprocess.run(["base64"] + args, input=data, capture_output=True)
    return p.stdout, p.returncode

def c_str(b):
    out = []
    for x in b:
        if x == 0x22: out.append('\\"')
        elif x == 0x5c: out.append("\\\\")
        elif 0x20 <= x < 0x7f: out.append(chr(x))
        else: out.append("\\%03o" % x)   # three digits, so a digit after it cannot join in
    return '"' + "".join(out) + '"'

cases = []   # (kind, arg, input, output, ok)

def add(kind, arg, data):
    args = {"e": ["-w", arg], "d": ["-d"], "i": ["-d", "-i"]}[kind]
    out, st = gnu(args, data)
    cases.append((kind, arg, data, out, st == 0))

# encoding: every length around the groups of three and a few lines' worth, each wrap
WRAPS = ["76", "0", "1", "2", "3", "4", "5", "7", "8", "10", "76", "100", "4294967295", "4294967296", "99999999999999999999", "9223372036854775807", "9223372036854775808", "18446744073709551615",
         "18446744073709551616", "-9223372036854775808", "+99999999999999999999", "00000000000000000000009", "2147483648",
         "007", "+4", " 4", "\t4", "\n4", "\x0b4", "\x0c4", "\r4", "\x1f4", "!4", "-0", "-00", "+0", "0x10", "5x", "4 ", "", "+", "-", "-1", "-5", "1e3", "٣"]
for w in WRAPS:
    for n in [0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 55, 56, 57, 58, 100]:
        add("e", w, bytes(random.randrange(256) for _ in range(n)))
for _ in range(300):
    add("e", random.choice(["76", "76", "1", "3", "4", "6", "9", "0", "20"]), bytes(random.randrange(256) for _ in range(random.randrange(0, 200))))
add("e", "76", b"\0" * 57); add("e", "76", b"\0" * 58); add("e", "76", bytes(range(256)) * 3)

# decoding
def good(n):
    out = "".join(random.choice(ALPHA) for _ in range(n))
    return out

for _ in range(3500):
    n = random.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 16, 17, 30, 31, 32, 33])
    r = random.random()
    if r < 0.15:
        # a real encoding, perhaps broken afterwards
        raw = bytes(random.randrange(256) for _ in range(random.randrange(0, 40)))
        s = subprocess.run(["base64", "-w", random.choice(["0", "76", "5", "4", "3"])], input=raw, capture_output=True).stdout.decode()
        if random.random() < 0.5 and s:
            s = list(s)
            for _ in range(random.choice([1, 1, 2])):
                s[random.randrange(len(s))] = random.choice(POOL)
            s = "".join(s)
    elif r < 0.55:
        s = list(good(n))
        for _ in range(random.choice([0, 0, 1, 2])):
            if s: s[random.randrange(len(s))] = random.choice(POOL)
        s = "".join(s)
    else:
        s = "".join(random.choice(POOL) for _ in range(n))
    data = s.encode("latin-1", "replace")
    add("d", "", data)
    add("i", "", data)
for s in ["", "\n", "\n\n", "YQ==", "YQ==\n", "YWI=", "YWJj", "YQ=", "Y", "YQ", "YQ=\n=", "YQ==YQ==", "YQ==\nYQ==", "YWJj\nZGVm\n", "YW\nJj", "YWJ\nj", "====",
          "=Y==", "Y=Q=", "YQ=A", "YQA=", "YWJjZA==", "YWJjZA=", "YWJjZA", "YWJjZ", "aGVsbG8", "ab=", "ab==", "abc=", "abc==", "ab=c", "a=bc", "a\nb=="]:
    add("d", "", s.encode())
    add("i", "", s.encode())

with open("tests/b64_cases.h", "w") as f:
    f.write("// Made by tests/gen_b64_cases.py: base64 input and what GNU base64 wrote for it.\n")
    f.write("// kind: 'e' encode with -w ARG, 'd' decode, 'i' decode with -i. ok: GNU's status was 0.\n")
    f.write("typedef struct { char kind; const char* arg; const char* in; unsigned inlen; const char* out; unsigned outlen; int ok; } b64_case_t;\n")
    f.write("static const b64_case_t b64_cases[] = {\n")
    for kind, arg, data, out, ok in cases:
        f.write('    { \'%s\', %s, %s, %d, %s, %d, %d },\n' % (kind, c_str(arg.encode("utf-8")), c_str(data), len(data), c_str(out), len(out), 1 if ok else 0))
    f.write("};\n")
print(len(cases), "cases")

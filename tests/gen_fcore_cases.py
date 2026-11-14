#!/usr/bin/env python3
"""Writes fcore_cases.h: numbers below 2^64 with what GNU factor made of them
(one line each, "12: 2 2 3"), and strings with what GNU makes of them as
arguments. Needs GNU factor (/usr/bin/factor). The generated file is checked
in, so test_fcore needs nothing but a compiler; rerun this only to change the
cases.

The numbers are the awkward ones for the arithmetic: every number up to 3000;
primes and products of primes just below 2^32 and 2^64 (the first number that
needs the long multiplication is 2^32 + 1); squares and higher powers of
primes; numbers with two or three equal-sized factors; Carmichael numbers and
strong pseudoprimes (what Miller-Rabin is most easily fooled by); numbers
around powers of two; and a seeded random draw of every bit length."""
import random, re, subprocess

GNU = "/usr/bin/factor"
M64 = 2**64
random.seed(20240607)

def is_prime(n):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True

def prev_prime(n):
    while not is_prime(n):
        n -= 1
    return n

def next_prime(n):
    while not is_prime(n):
        n += 1
    return n

def rand_prime(bits):
    return next_prime(random.getrandbits(bits) | 1 << (bits - 1))

nums = set(range(0, 3001))
for b in range(0, 65):
    for d in (-2, -1, 0, 1, 2):
        nums.add(2**b + d)
nums.add(M64 - 1)
for k in range(40):
    nums.add(prev_prime(M64 - 1 - k * 997))
    nums.add(prev_prime(2**63 - k * 991))
    nums.add(next_prime(2**32 + k * 1009))
    nums.add(prev_prime(2**32 - 1 - k * 1013))
big = prev_prime(2**32 - 1)
nums |= {big * big, big * prev_prime(big - 1), big * 3}
for bits in (16, 17, 20, 21, 22, 31, 32):
    for _ in range(30):
        p = rand_prime(bits)
        for e in range(2, 64 // bits + 1):
            nums.add(p ** e)
        q = rand_prime(bits)
        nums.add(p * q)
        nums.add(p * p * q if p * p * q < M64 else p * q)
for _ in range(120):
    a, b = rand_prime(32), rand_prime(32)
    nums.add(a * b)
    nums.add(a * rand_prime(31))
for _ in range(80):
    ps = [rand_prime(random.choice((16, 20, 21))) for _ in range(random.choice((3, 3, 4)))]
    n = 1
    for p in ps:
        n *= p
    if n < M64:
        nums.add(n)
# Carmichael numbers (every base passes Fermat's test) and strong pseudoprimes
nums |= {561, 1105, 1729, 2465, 2821, 6601, 8911, 10585, 15841, 29341, 41041, 46657, 52633, 62745, 63973, 75361,
         101101, 115921, 126217, 162401, 172081, 188461, 252601, 278545, 294409, 314821, 334153, 340561, 399001,
         410041, 449065, 488881, 512461, 3215031751, 2152302898747, 3474749660383, 341550071728321, 3825123056546413051,
         1122004669633, 4759123141, 1000000007 * 998244353, 1000000007 ** 2,
         6700417 * 641, 274177 * 67280421310721 % M64, 4294967297, 4294967291 * 4294967279}
for _ in range(80):  # Carmichael-like: (6k+1)(12k+1)(18k+1)
    k = random.randrange(1, 1 << 14)
    ps = (6 * k + 1, 12 * k + 1, 18 * k + 1)
    if all(is_prime(p) for p in ps):
        nums.add(ps[0] * ps[1] * ps[2])
p = 1
for q in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53):  # primorials and their neighbours
    p *= q
    for d in (-1, 0, 1):
        if 0 <= p + d < M64:
            nums.add(p + d)
for _ in range(2500):
    nums.add(random.getrandbits(random.randint(1, 64)))
nums = sorted(n for n in nums if 0 <= n < M64)

def gnu(lines):
    p = subprocess.run([GNU], input=("\n".join(lines) + "\n").encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={"LC_ALL": "C"}, timeout=600)
    return p.stdout.decode().splitlines(), p.stderr.decode().splitlines()

out, err = gnu([str(n) for n in nums])
assert not err and len(out) == len(nums)
for n, line in zip(nums, out):
    assert line.startswith("%d:" % n)

# what an argument is made of: GNU takes an optional + and digits, nothing else
toks = ["", "+", "-", "-1", "+1", "++1", "+-1", "1+", "1 ", " 1", "0", "00", "+0", "007", "+007", "12", "0x10", "1e3", "1.0", "1,2", "abc", "12a", "a12", "1_0", "٣", "\t1", "1\r", "  12", " +12", "+ 12", " ", "  ", " + ", " - ", " -1", "\v1", "\f1", "\n1", "1\n", "+  1", "   +   1", "  +00018446744073709551615", "  18446744073709551616",
        "18446744073709551615", "+18446744073709551615", "0018446744073709551615", "18446744073709551616", "18446744073709551617", "+18446744073709551616", "18446744073709551620", "18446744073709551625",
        "99999999999999999999", "184467440737095516150", "1844674407370955161", "1844674407370955162", "100000000000000000000000000000", "00000000000000000000000000000000000000012"]
toks += [str(random.getrandbits(random.randint(60, 70))) for _ in range(40)]
toks += ["0" * k + "1" for k in range(1, 30)]
parse = []
for t in toks:
    p = subprocess.run([GNU, b"--", t.encode()], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={"LC_ALL": "C"}, timeout=60)
    ok = p.returncode == 0
    assert ok or b"is not a valid positive integer" in p.stderr, (t, p.stderr)
    if ok:
        v = int(t)  # GNU goes beyond 2^64: ours refuses that, as too big
        parse.append((t, 0 if v < M64 else 2, v if v < M64 else 0))
        if v < M64:
            assert p.stdout.decode().startswith("%d:" % v)
    else:
        parse.append((t, 1, 0))

def cstr(s):
    return '"' + "".join(chr(c) if 32 <= c < 127 and chr(c) not in '"\\?' else "\\%03o" % c for c in s.encode("utf-8")) + '"'

with open(__file__.rsplit("/", 1)[0] + "/fcore_cases.h", "w") as f:
    f.write("// Generated by gen_fcore_cases.py from GNU factor's output. Do not edit.\n")
    f.write("typedef struct { unsigned long long n; const char* line; } fc_case_t;\n")
    f.write("static const fc_case_t fc_cases[] = {\n")
    for n, line in zip(nums, out):
        f.write('    { %dull, "%s" },\n' % (n, line))
    f.write("};\n")
    f.write("// FC_OK 0, FC_INVALID 1, FC_TOO_BIG 2\n")
    f.write("typedef struct { const char* s; int result; unsigned long long val; } fc_parse_case_t;\n")
    f.write("static const fc_parse_case_t fc_parse_cases[] = {\n")
    for t, r, v in parse:
        f.write("    { %s, %d, %dull },\n" % (cstr(t), r, v))
    f.write("};\n")
print(len(nums), "numbers,", len(parse), "tokens")

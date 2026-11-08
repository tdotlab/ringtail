#!/usr/bin/env python3
"""Writes regex_cases.h: patterns, flags, and what GNU grep (LC_ALL=C) makes of
them over a fixed set of lines. Needs GNU grep. The generated file is checked in,
so test_regex needs nothing but a compiler; rerun this only to change the cases.

For every case it records which lines `grep -n` selects and the extents `grep -o -b`
prints. Random patterns come from a seeded generator and use ^ and $ only at the
ends of a branch (GNU treats an anchor in the middle of an ERE oddly, and we do not
copy that)."""
import os, random, subprocess, sys, tempfile

LINES = ["", "a", "b", "ab", "abc", "aab", "abab", "cab", "a b", "ab ab", "a_b", "  ",
         "aaa", "abcabc", "ABC", "aBc", "b a", "ba", "bab", "cc", "a.b", "a*b", "a+b",
         "(a)", "a|b", "x{2}", "aa bb", "abba", "c", "c c", "foo bar baz", "The quick fox",
         "tab\there", "1234 5678", "a\\b", "^caret", "dollar$"]

# Hand-written: the cases that say what the syntax means.
FIXED = [
    # (flags, pattern)   flags: E ere, F fixed, i icase, w word, x line
    ("", "abc"), ("", "a.c"), ("", "a*"), ("", "*a"), ("", "^*a"), ("", "a\\{2\\}"),
    ("", "a\\{1,\\}b"), ("", "\\(ab\\)*c"), ("", "\\(a\\|b\\)\\+"), ("", "a\\?b"),
    ("", "^a"), ("", "b$"), ("", "^$"), ("", "^^caret"), ("", "dollar$$"), ("", "a^b"),
    ("", "[abc]"), ("", "[^abc]"), ("", "[a-c]*c"), ("", "[]a]"), ("", "[^]a]"), ("", "[a-]"),
    ("", "[[:digit:]]\\+"), ("", "[[:upper:]][[:lower:]]"), ("", "[[:space:]]"), ("", "[[:alpha:]_]*"),
    ("", "\\w\\+"), ("", "\\W"), ("", "\\s"), ("", "\\S\\+"), ("", "\\bfoo"), ("", "bar\\b"),
    ("", "\\Bar"), ("", "\\<q"), ("", "x\\>"), ("", "a\\.b"), ("", "a\\*b"), ("", "a|b"),
    ("", "(a)"), ("", "x{2}"), ("", "a\\|b\\|c"), ("", "\\(a*\\)*b"), ("", "\\(a\\|ab\\)\\(c\\|bcd\\)*"),
    ("E", "a|b"), ("E", "ab|a"), ("E", "(ab)+"), ("E", "a{2}"), ("E", "a{1,2}b"), ("E", "a{,2}b"),
    ("E", "x{2}"), ("E", "a{"), ("E", "a{x}"), ("E", "(a|ab)(c|bcd)?"), ("E", "(a*)*b"),
    ("E", "(a+)+c"), ("E", "^(a|b)+$"), ("E", "a?"), ("E", "+a"), ("E", "()"), ("E", "a||b"),
    ("E", "(^a|b$)"), ("E", "[0-9]+ [0-9]+"), ("E", "\\(a\\)"), ("E", "a\\{2\\}"), ("E", "\\|"),
    ("E", "a**"), ("E", "(a?){3}a{3}"), ("E", "b{0}a"), ("E", "(ab){2,}"), ("E", "[[:alpha:]]+ [[:alpha:]]+"),
    ("i", "abc"), ("i", "A.C"), ("i", "[a-b]c"), ("i", "[^a]"), ("Ei", "(AB)+"),
    ("F", "a.b"), ("F", "a*b"), ("F", "a|b"), ("F", "(a)"), ("F", "ab"), ("F", "[a]"), ("F", "\\"), ("Fi", "AB"),
    ("F", "a\nb"), ("", "a\nb"), ("E", "ab\nc$"), ("", ""), ("E", ""), ("F", ""),
    ("w", "a"), ("w", "ab"), ("w", "a.*"), ("w", "foo bar"), ("w", "[a-z]*"), ("Ew", "a|ab"), ("Ew", "a+"),
    ("w", "_"), ("w", "123"), ("wi", "ABC"), ("Fw", "a"),
    ("x", "a"), ("x", "ab"), ("x", "a.*"), ("x", ""), ("Ex", "a|ab"), ("Ex", "(ab)+"), ("Fx", "ab"), ("xi", "ABC"),
    ("Ex", "a*"), ("wx", "ab"),
    # errors
    ("", "\\{1\\}a"), ("", "\\+a"), ("", "\\?a"), ("", "\\(\\{1\\}a\\)"), ("E", "+a"), ("E", "*a"), ("E", "?a"), ("E", "(+a)"), ("E", "a|*b"),
    ("E", "a)"), ("E", "(a))"), ("", "\\("), ("", "\\)"), ("", "a\\{1"), ("", "a\\{2,1\\}"), ("", "[a"), ("", "[[:foo:]]"), ("", "[b-a]"),
    ("E", "("), ("E", ")"), ("E", "a{2,1}"), ("E", "[a"), ("E", "a{1"), ("", "\\{1\\}"), ("", "a\\"), ("E", "a\\"),
    ("E", "(a|b"), ("", "\\(a\\|b"), ("", "[[:alpha:]"), ("", "[[:alpha:"),
]

def atom(ere, depth):
    r = random.random()
    if r < 0.45: return random.choice("abcab ")
    if r < 0.55: return "."
    if r < 0.65: return random.choice(["[ab]", "[^a]", "[a-c]", "[[:alpha:]]", "[[:space:]]", "[]a]", "[^]a]", "[a-]", "[b-]"])
    if r < 0.70: return random.choice([r"\w", r"\W", r"\s", r"\S", r"\b", r"\B", r"\<", r"\>"])
    if r < 0.78: return random.choice(["\\.", "\\*", "\\[", "\\^", "\\$", "\\\\"])
    if depth < 3:
        inner = alt(ere, depth + 1, False)
        return ("(%s)" if ere else r"\(%s\)") % inner
    return "a"

def post(ere):
    r = random.random()
    if r < 0.55: return ""
    if r < 0.70: return "*"
    if r < 0.80: return "+" if ere else r"\+"
    if r < 0.90: return "?" if ere else r"\?"
    m = random.randint(0, 2); n = random.choice([m, m + 1, m + 2, ""])
    s = "%d,%s" % (m, n) if random.random() < 0.8 else "%d" % m
    return "{" + s + "}" if ere else "\\{" + s + "\\}"

ASSERT = (r"\b", r"\B", r"\<", r"\>")

def seq(ere, depth, top):
    out = "^" if top and random.random() < 0.15 else ""
    for _ in range(random.randint(1, 4)):
        a = atom(ere, depth)
        out += a if a in ASSERT else a + post(ere)
    if top and random.random() < 0.15: out += "$"
    return out

def alt(ere, depth, top):
    parts = [seq(ere, depth, top) for _ in range(random.choice([1, 1, 1, 2, 3]))]
    return ("|" if ere else r"\|").join(parts)

# GNU grep disagrees with itself on these (-n selects a line that -o finds nothing in),
# so there is nothing to copy.
GNU_QUIRKS = {("E", r"^\Waa*b*|^(\<[[:alpha:]](c)?){1,2}a")}

def gnu(flags, pat, path):
    base = ["grep"] + (["-E"] if "E" in flags else ["-F"] if "F" in flags else ["-G"]) + \
           ["-" + c for c in "iwx" if c in flags]
    env = {"LC_ALL": "C"}
    try:
        sel = subprocess.run(base + ["-n", "-e", pat, path], capture_output=True, env=env, timeout=3)
        out = subprocess.run(base + ["-n", "-o", "-b", "-e", pat, path], capture_output=True, env=env, timeout=3)
    except subprocess.TimeoutExpired:
        return None
    if sel.returncode == 2: return "ERR"
    selected = [int(l.split(b":", 1)[0]) - 1 for l in sel.stdout.split(b"\n") if l]
    starts, p = [], 0
    for l in LINES: starts.append(p); p += len(l) + 1
    per = {}
    for l in out.stdout.split(b"\n"):
        if not l: continue
        ln, off, txt = l.split(b":", 2)
        k = int(ln) - 1
        per.setdefault(k, []).append("%d-%d" % (int(off) - starts[k], int(off) - starts[k] + len(txt)))
    return "sel=" + ",".join(map(str, selected)) + " o=" + ";".join("%d:%s" % (k, ",".join(v)) for k, v in sorted(per.items()))

def cstr(s):
    return '"' + "".join({"\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t"}.get(c, c) for c in s) + '"'

def main():
    random.seed(20240607)
    cases = list(FIXED)
    for _ in range(700):
        ere = random.random() < 0.5
        fl = ("E" if ere else "") + (random.choice("iwx") if random.random() < 0.4 else "")
        cases.append((fl, alt(ere, 0, True)))
    d = tempfile.mkdtemp()
    path = os.path.join(d, "corpus")
    open(path, "w").write("\n".join(LINES) + "\n")
    rows, seen = [], set()
    for fl, pat in cases:
        if (fl, pat) in seen or (fl, pat) in GNU_QUIRKS: continue
        seen.add((fl, pat))
        exp = gnu(fl, pat, path)
        if exp is None: continue
        rows.append((fl, pat, exp))
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "regex_cases.h"), "w") as f:
        f.write("// Generated by gen_regex_cases.py from GNU grep's answers. Do not edit.\n")
        f.write("static const char* const corpus[] = {\n")
        for l in LINES: f.write("    %s,\n" % cstr(l))
        f.write("};\n\ntypedef struct { const char* flags; const char* pat; const char* want; } rcase_t;\n")
        f.write("static const rcase_t cases[] = {\n")
        for fl, pat, exp in rows:
            f.write("    { %s, %s, %s },\n" % (cstr(fl), cstr(pat), cstr(exp)))
        f.write("};\n")
    print(len(rows), "cases,", sum(1 for r in rows if r[2] == "ERR"), "errors")

main()

#!/usr/bin/env python3
"""Makes tests/text_cases.py: shell commands for the text tools (cut, tr,
tee, ...) together with what the real GNU tools print for them. The QEMU
test types each command into RingTail's shell and compares.

Every command is plain enough to mean the same to bash and to RingTail's
shell: quotes, pipes, echo -e, redirections. A command starting
with "?" may fail (the test strips the "?"; the shell then prints the
exit status, which is part of the expected output). Each case is a list of
commands run one after another in an empty directory, with what each one
printed on standard output (the test ignores trailing newlines).

Run from the repository root:  python3 tests/gen_text_cases.py
"""
import os, subprocess, sys, tempfile

CASES = []

def case(*cmds):
    CASES.append(cmds)

# ---------------------------------------------------------------- cut
T = "echo -e 'a:b:c:d\\nxx:yy\\n\\n:lead\\ntrail:\\nnone' | "
for lst in ["1", "2", "1,3", "2-3", "2-", "-2", "1-2,4", "3,1", "4-9", "1-1", "2,2,2", "1-2,2-3", "5", "9"]:
    case(T + f"cut -d : -f {lst}")
    case(T + f"cut -d: -f{lst}")
    case(T + f"cut -d : -s -f {lst}")
    case(T + f"cut -d : --complement -f {lst}")
B = "echo -e 'abcdefgh\\nxy\\n\\n0123456789' | "
for lst in ["1", "3", "1,3,5", "2-4", "6-", "-3", "1-2,5-6", "1-3,3-5", "9", "1-2,3-4", "100", "4-4,2-2"]:
    case(B + f"cut -b {lst}")
    case(B + f"cut -c {lst}")
    case(B + f"cut -c{lst}")
    case(B + f"cut -c {lst} --complement")
case(B + "cut -b 1-2,5-6 --output-delimiter=-")
case(B + "cut -b 1-2,3-4 --output-delimiter=-")
case(B + "cut -b 1,3,5 --output-delimiter=:")
case(B + "cut -c 1-2,5- --output-delimiter=,,")
case(T + "cut -d : -f 1,3 --output-delimiter=-")
case(T + "cut -d : -f 1-3 --output-delimiter=")
case("echo -e 'a\\tb\\tc' | cut -f 2")
case("echo -e 'a\\tb\\tc' | cut -f 1,3")
case("echo -e 'a b c' | cut -d ' ' -f 2")
case("echo -e 'a,b;c' | cut -d ';' -f 1")
case("echo -e 'a-b-c' | cut -d - -f 2-")
case("echo -e 'a b c\\c' | cut -d ' ' -f 2,3")
case("echo -e 'abc\\c' | cut -c 2")
case("echo -e 'one\\ntwo' > cf1", "echo -e 'three\\nfour' > cf2", "cut -c 1-2 cf1 cf2")
case("echo -e 'one\\ntwo' > cf1", "cut -c 2 cf1 - < cf1")
case("echo -e 'a:b:c' | cut -f 1 -d : -s")
case("echo -e 'a:b:c' | cut -s -d : -f 4")
case("echo -e 'a:b:c' | cut -d : -f 2,1")
case("echo -e '::' | cut -d : -f 2")
case("echo -e '::' | cut -d : -f 4")
case("echo -e '::' | cut -d : -f 1-3")
case("echo -e 'a:b' | cut -d : --complement -f 1-")
case("echo -e 'a:b' | cut -d : --complement -f 3-")
case("echo -e 'a\\0ab' | cut -d : -f 1 | wc -c")

# ----------------------------------------------------------------- tr
X = "echo -e 'Hello, World 123\\nfoo  bar\\ttab' | "
trs = [
    "a-z A-Z", "A-Z a-z", "lo 01", "'[:lower:]' '[:upper:]'", "'[:upper:]' '[:lower:]'", "a-c x", "a-c xy",
    "abc xyz123", "'[:digit:]' '#'", "'[:space:]' _", "'[:alpha:]' '.'", "o 0", "lo '[x*]'", "'a-e' '[X*3]Y'",
    "o '\\n'", "' ' '\\t'", "'\\t' ' '", "'\\n' ' '", "a-y b-z", "'[=l=]' L", "'0-9' 'a-j'", "'[:alnum:]' '*'",
    "'[:punct:]' '!'", "'[:xdigit:]' 'x'", "'\\150' H", "o '\\157'", "'[a-c]' 'x'", "'a-cx-z' '12345'",
    "-c a-z '*'", "-c 'a-zA-Z' '_'", "-c '[:alpha:]' '\\n'", "-c a '[b*]'", "-ct a-c xy", "-t abc xy", "-t abc x",
    "-d o", "-d lo", "-d '[:digit:]'", "-d '[:space:]'", "-d '\\n'", "-d a-z", "-dc a-z", "-dc '[:alnum:]'", "-d '\\t'", "-d '[:upper:]'",
    "-s ' '", "-s o", "-s 'o ' ", "-s '[:space:]'", "-s a-z", "-s 'a-z' 'A-Z'", "-s ' ' '_'", "-s ' ' '\\n'", "-s 'o' '0'",
    "-cs a-zA-Z '\\n'", "-cs '[:alpha:]' '\\n'", "-sc a-z '+'", "-ds a-z ' '", "-ds 'o' ' '", "-ds 'o' 'l'", "-ds '[:digit:]' 'o'",
    "-s lo 01", "-s a-z 'x'", "-s '\\n' ' '",
]
for a in trs:
    case(X + f"tr {a}")
case("echo -e 'a-b' | tr - _")
case("echo -e 'a-b' | tr 'a-' 'xy'")
case("echo -e 'a\\\\b' | tr '\\\\' '/'")
case("echo -e 'a\\\\b' | tr '\\\\\\\\' '/'")
case("echo -e 'aabb' | tr aa xy")
case("echo -e 'abc' | tr 'abc' 'xxx'")
case("echo -e 'abc' | tr -s 'abc' 'x'")
case("echo -e 'a[b]c' | tr '[]' '()'")
case("echo -e 'a*b' | tr '*' '+'")
case("echo -e 'abc' | tr 'a-c' '[A*]'")
case("echo -e 'abc' | tr 'a-c' '[A*2]z'")
case("echo -e 'abc' | tr 'a-c' '[\\n*]'")
case("echo -e 'a b' | tr '[:blank:]' '-'")
case("echo -e 'a\\tb c' | tr '[:blank:]' '-'")
case("echo -e 'tab\\there' | tr '[:cntrl:]' '?'")
case("echo -e 'x y' | tr '[:graph:]' '#'")
case("echo -e 'x y' | tr '[:print:]' '#'")
case("echo -e 'abc\\c' | tr a-c A-C")
case("echo -e '' | tr a b")
case("echo -e 'abc' | tr -d ''")
case("echo -e 'aXb' | tr -dc a-z")
case("echo -e 'a1b2c3' | tr -d 0-9")
case("echo -e 'a1b2c3' | tr -cd 0-9")
case("echo -e 'a1b2c3' | tr -cd '0-9\\n'")
case("echo -e 'a b  c   d' | tr -s ' ' | cut -d ' ' -f 2-3")
case("echo -e 'aaa bbb' | tr -s a b")
case("echo -e 'Hello' | tr A-Za-z N-ZA-Mn-za-m")
case("echo -e 'Uryyb' | tr A-Za-z N-ZA-Mn-za-m")

# ---------------------------------------------------------------- tee
case("echo hi | tee t1", "cat t1")
case("echo hi | tee t1 t2", "cat t1", "cat t2")
case("echo one | tee t1", "echo two | tee -a t1", "cat t1")
case("echo one | tee t1", "echo two | tee t1", "cat t1")
case("echo one | tee -a t1 t2", "echo two | tee -a t1 t2", "cat t2")
case("echo hi | tee", )
case("echo -e 'a\\nb\\nc' | tee t1 | wc -l", "cat t1")
case("echo -e 'a\\nb\\nc' | tee - | wc -l")
case("echo -e 'a\\nb\\nc' | tee t1 | tr a-c A-C", "cat t1")
case("echo -e 'a\\nb' | tee t1 | cut -c 1 | tee t2", "cat t2")
case("echo -e 'abc\\c' | tee t1", "cat t1")

# ----------------------------------------------------------- basename
for a in ["/usr/bin/sort", "sort", "/usr/bin/", "/", "//", "a//b", "a/b/", "a/b///", "", "x.c", ".c", ".", "..", "a/.", "/a", "//a//b//"]:
    case(f"basename '{a}'")
    case(f"dirname '{a}'")
for a in ["x.c .c", "x.c x.c", "x.c c", "/a/b.txt .txt", "/a/b.txt b.txt", "/a/b.txt txt", "a/b/ b", "ab b", "aab ab", "abab ab", "a.b.c .c", "xx ''"]:
    case(f"basename {a}")
case("basename -a /a/b /c/d e")
case("basename -s .c /a/x.c /b/y.c z.c")
case("basename -s.c /a/x.c")
case("basename -a -s .o x.o y.o")
case("basename -as .o x.o y.o")
case("basename -a ''")
case("dirname /a/b /c d/e/ f")
case("dirname /a/b/c/d.txt")
case("dirname a/b/c")
case("dirname .hidden")
case("dirname /usr/")
case("basename -- -x")
case("basename -- -x x")

# ---------------------------------------------------------------- seq
for a in ["3", "1", "0", "-1", "1 3", "3 1", "5 5", "-2 2", "2 -2", "1 2 10", "10 -3 1", "10 -3 0", "1 3 1",
          "0 0.5 2", "1 0.25 2", "0.5 2", "1.5 3", "-1 0.5 1", "2 -0.5 0", "0.1 0.1 0.5", "1.0 3", "1 1.0 3", "1 .5 3", "00 02", "007 009", "+1 +3", "1 5 20",
          "-5 -1", "-1 -5", "100 100 500", "1.25 0.25 2", "0 0.001 0.003", "1000000 1000002", "-0.5 0.5 1", "3 1 3", "1 1 0"]:
    case(f"seq {a}")
    case(f"seq -w {a}")
for a in ["1 0.5 03", "01 0.5 3", "01.5 3", "1.5 003", "1 2 010", "-1 01", "-01 1", "+01 3", "1 0.5 10", "9 0.5 10"]:
    case(f"seq -w {a}")
for a in ["1 5", "1 2 9", "5 -1 1", "0 0.5 2"]:
    case(f"seq -s , {a}")
    case(f"seq -s: {a}")
    case(f"seq -w -s ' ' {a}")
    case(f"seq -s '' {a}")
    case(f"seq -s ', ' {a}")
case("seq 1 3 | tr '\\n' ' '")
case("seq 20 | tail -n 2")
case("seq 100 | wc -l")
case("seq 1000 | tail -n 1")
case("seq 5 | sort -r | head -n 2")
case("seq 10 | grep 1")

# ---------------------------------------------------------------- cmp
case("echo -e 'abc' > c1", "echo -e 'abc' > c2", "cmp c1 c2")
case("echo -e 'abc' > c1", "echo -e 'abd' > c2", "?cmp c1 c2")
case("echo -e 'abc\\ndef' > c1", "echo -e 'abc\\ndxf' > c2", "?cmp c1 c2")
case("echo -e 'abc\\ndef' > c1", "echo -e 'abc\\ndxf' > c2", "?cmp -s c1 c2")
case("echo -e 'abc\\ndef' > c1", "echo -e 'abc\\ndef' > c2", "cmp -s c1 c2")
case("echo -e 'abc\\ndef' > c1", "echo -e 'xbc\\ndxf' > c2", "?cmp -l c1 c2")
case("echo -e 'abcdefghijkl' > c1", "echo -e 'aXcdefghijkL' > c2", "?cmp -l c1 c2")
case("echo -e 'abcdef' > c1", "echo -e 'abcdXf' > c2", "?cmp -l c1 c2")
case("echo -e 'abcdef' > c1", "echo -e 'abcdXf' > c2", "cmp -n 4 c1 c2")
case("echo -e 'abcdef' > c1", "echo -e 'abcdXf' > c2", "?cmp -n 5 c1 c2")
case("echo -e 'abcdef' > c1", "echo -e 'abcdXf' > c2", "cmp -n4 c1 c2")
case("echo -e 'abcdef' > c1", "echo -e 'abcdXf' > c2", "?cmp -ln 6 c1 c2")
case("echo -e 'abc' > c1", "echo -e 'abc' | cmp c1 -")
case("echo -e 'abc' > c1", "?echo -e 'abd' | cmp - c1")
case("echo -e 'abc' > c1", "?echo -e 'abd' | cmp c1")
case("echo -e '\\0001\\0377' > c1", "echo -e '\\0001\\0376' > c2", "?cmp -l c1 c2")
case("echo -e 'a' > c1", "echo -e 'b' > c2", "?cmp -l c1 c2")
case("echo -e 'abc' > c1", "echo -e 'abc' > c2", "cmp -l c1 c2")
case("echo -e 'a\\n\\n\\nb' > c1", "echo -e 'a\\n\\n\\nc' > c2", "?cmp c1 c2")
case("echo -e 'abc' > c1", "?cmp c1 c1")

def run_all():
    out, bad = [], []
    for cmds in CASES:
        d = tempfile.mkdtemp(prefix="gen-text-")
        parts = []
        for c in cmds:
            may_fail = c.startswith("?")
            p = subprocess.run(["bash", "-c", c.lstrip("?")], cwd=d, capture_output=True, timeout=10)
            if p.returncode and not may_fail:
                bad.append(f"{c!r}: {p.stderr.decode().strip()}")
                break
            # a program that fails makes RingTail's shell print its status
            # older diffutils say "char" where newer ones say "byte"
            parts.append(p.stdout.decode("latin-1").replace(" differ: char ", " differ: byte ") + (f"[exit status {p.returncode}]" if p.returncode else ""))
        out.append((cmds, tuple(parts)))
    if bad:
        sys.exit("GNU refused:\n  " + "\n  ".join(bad))
    return out

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    cases = run_all()
    with open(os.path.join(here, "text_cases.py"), "w") as f:
        f.write("# Generated by gen_text_cases.py from GNU coreutils' output. Do not edit.\n")
        f.write("CASES = [\n")
        for cmds, exp in cases:
            f.write(f"    ({cmds!r}, {exp!r}),\n")
        f.write("]\n")
    print(f"{len(cases)} cases")

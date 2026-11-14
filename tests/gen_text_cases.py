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
import os, re, subprocess, sys, tempfile

CASES = []
ERRS = []  # sed scripts: the test checks the exit status and that a message came out

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

# ---------------------------------------------------------------- sed
W = "echo -e 'one\\ntwo\\nthree\\nfour\\nfive\\nsix\\nseven\\neight' | "
for sc in ["p", "d", "2d", "2,4d", "$d", "1~3d", "2~2p", "0~4p", "2,+2d", "2,~4d", "/t/d", "/t/,/f/d", "/e$/!d", "3q", "3Q",
           "2!d", "1!G;h;$!d", "$!N;s/\\n/+/", "n;d", "N;P;D", "2,3!d", "/two/,3p", "4,2p", "2,/e/d", "0,/o/d", "1,/o/d",
           "/one/,/one/d", "y/abcdefghij/ABCDEFGHIJ/", "=", "l", "2=", "2{p;p}", "2,3{s/^/> /}", "/t/{/w/d}", "$!d;=",
           "1h;2,$H;$!d;g", "x;$G", "2z", "G", "1d;$d", "/O/I!d", "\\,t,d", "/o/s//0/", "s/o/0/2", "s/o/0/g",
           "s/\\(.\\)\\(.\\)/\\2\\1/", "s/./&&/", "s/e/E/3"]:
    case(W + f"sed '{sc}'")
    case(W + f"sed -n '{sc}'")
for sc in ["s/o/0/", "s/o/0/g", "s/o/0/2", "s/o/0/2g", "s/O/0/I", "s/O/0/gi", "s/^/X/", "s/$/X/", "s/e*/-/g", "s/x*/-/g", "s/[aeiou]/(&)/g",
           "s/\\w\\+/[&]/", "s/\\bt/T/", "s/o\\|e/_/g", "s/t\\?e/#/", "s/e\\{1,2\\}/#/", "s|o|/|", "s,o,\\,,", "s/o/\\//", "s/o/a\\&b/",
           "s/.*/\\L&/", "s/\\(.\\)/\\u\\1/g", "s/./\\l&/", "s/[^e]*e//", "s/^.*\\(.\\)$/\\1/", "s/o/&&/g;s/e/E/", "s/./x/3g", "s/ //g",
           "s/\\./,/g", "s/[[:upper:]]/u/g", "s/\\s//", "s/\\t/T/", "s/e/\\t/", "s/*/x/", "s/a*/x/g"]:
    case(W + f"sed '{sc}'")
for sc in ["s/e/E/p", "s/e/E/gp", "s/x/y/p"]:
    case(W + f"sed -n '{sc}'")
case(W + "sed -n 's/e/E/w out'", "cat out")
case(W + "sed -e 2d -e 4d")
case(W + "sed -n -e 2p -e 4p")
case(W + "sed -e '2{' -e 'p' -e '}'")
case(W + "sed -n '2{p};4p'")
case(W + "sed -n '2 {p}'")
case(W + "sed -n '2p;4p;'")
case(W + "sed -n ' 2 , 4 p'")
case(W + "sed -n '2,4 !p'")
case(W + "sed -n '/t/ , /f/ p'")
case(W + "sed '2d;4d' | sed -n '$='")
case(W + "sed --quiet 3p")
case(W + "sed --expression=3d")
case(W + "sed -ne 3p")
case(W + "sed -n -e '/two/{n;p}'")
case(W + "sed -n '/two/{N;N;p}'")
case(W + "sed '$!N;$!D'")
case(W + "sed '/two/{N;N;d}'")
case(W + "sed 'N;N;s/\\n/,/g'")
case(W + "sed 'a\\hello'")
case(W + "sed '2a hello'")
case(W + "sed '2a\\  hello'")
case(W + "sed '2i hello'")
case(W + "sed '2c hello'")
case(W + "sed '2,4c hello'")
case(W + "sed '$a end'")
case(W + "sed '1i start'")
case(W + "sed -e '2a one' -e '2a two'")
case(W + "sed '2a hello\\nworld'")
case(W + "sed -n '2a hello'")
case(W + "sed -e '2{a X' -e '}'")
case("echo -e '\\c' > e0", W + "sed '2r e0'")
case("echo -e 'A\\nB' > ins", W + "sed '2r ins'")
case("echo -e 'A\\nB' > ins", W + "sed '$r ins'")
case("echo -e 'A\\nB' > ins", W + "sed '/t/r ins'")
case("echo -e 'A\\nB' > ins", W + "sed '2r nosuchfile'")
case(W + "sed -n '2{p;q}'")
case("?" + W + "sed '2q5'")
case("?" + W + "sed '2Q7'")
case(W + "sed 5q")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -n '$p' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -s -n '$p' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -n '1p' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -s -n '1p' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -n '=' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -s -n '=' f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -n '$p' f1 - f2 < f1")
case("echo -e 'a\\nb' > f1", "sed -i s/a/X/ f1", "cat f1")
case("echo -e 'a\\nb' > f1", "sed -i.bak s/a/X/ f1", "cat f1", "cat f1.bak")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -i 1d f1 f2", "cat f1 f2")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\nd' > f2", "sed -i -n '$p' f1 f2", "cat f1 f2")
case("echo -e 'a\\nb' > f1", "sed -n -i p f1", "cat f1")
case("echo -e 'a\\nb' > f1", "sed -i '$a end' f1", "cat f1")
case("echo -e 'a\\nb' > f1", "sed -i = f1", "cat f1")
case("echo -e 'a\\nb' > f1", "sed -i -e 1d -e 1d f1", "cat f1")
case("echo -e 'a\\nb' > f1", "echo -e 's/a/Z/\\n2d' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '#n\\n1p' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '1d\\n# note\\n\\n/b/ s/b/B/' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e 'a\\\\\\nZ' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '2i\\\\\\nfirst\\\\\\nsecond' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '2a\\\\\\n  keep' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '$!{\\nN\\ns/\\\\n/-/\\n}' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '2{\\ns/b/B/\\np\\n}' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e 's/a/x\\\\\\ny/' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e '2c\\\\\\nnew' > sc.sed", "sed -f sc.sed f1")
case("echo -e 'a\\nb' > f1", "echo -e ':a\\ns/^.\\\\{1,3\\\\}$/ &/\\nta' > sc.sed", "sed -f sc.sed f1")
case(W + "sed '#n'")
case(W + "sed -n -E 's/(t)(.)/\\2\\1/p'")
case(W + "sed -E 's/(.)(.)/\\2\\1/'")
case(W + "sed -r 's/(.)(.)/\\2\\1/'")
case(W + "sed -E 's/e+/#/g'")
case(W + "sed -E 's/(o|e)/_/g'")
case(W + "sed -E 's/^(.)(.*)(.)$/\\3\\2\\1/'")
case(W + "sed -E 's/(a|b)?c/X/'")
case(W + "sed -E 's/(e)|(o)/[\\1\\2]/g'")
case(W + "sed -E 's/(e|o)+/<\\1>/'")
case(W + "sed -E 's/(.*)(e)/\\2\\1/'")
case(W + "sed -E 's/(t|th)(r|h)?/[\\1,\\2]/'")
case(W + "sed -E 's/x?(o)?/[\\1]/'")
case(W + "sed -E 's/((.)(.))/\\3\\2/'")
case(W + "sed -E 's/(.)+/\\1/'")
case(W + "sed -E 's/o{1}/0/'")
case(W + "sed -E '/^(one|two)$/d'")
case(W + "sed 's/\\(o\\)/\\1\\1/'")
case(W + "sed 's/one/&&/;s/two/(&)/'")
case("echo -e 'a\\n\\nb\\n\\n\\nc' | sed '/^$/d'")
case("echo -e 'a\\n\\nb\\n\\n\\nc' | sed '/./,/^$/!d'")
case("echo -e 'a\\n\\nb\\n\\n\\nc' | sed -n '/^$/='")
case("echo -e 'a b  c\\td' | sed 's/[ \\t]\\+/_/g'")
case("echo -e 'a b  c' | sed 's/ /\\n/g'")
case("echo -e 'hello world' | sed 's/\\w\\+/\\u&/g'")
case("echo -e 'hello world' | sed 's/\\(hello\\) \\(world\\)/\\U\\1\\E \\u\\2/'")
case("echo -e 'hello world' | sed 's/o/0/2'")
case("echo -e 'hello world' | sed 's/l*/X/g'")
case("echo -e 'hello world' | sed 's/\\b/|/g'")
case("echo -e 'hello world' | sed 's/\\B/|/g'")
case("echo -e 'hello world' | sed 's/\\</</g;s/\\>/>/g'")
case("echo -e 'hello world' | sed 's/.\\{3\\}/&-/g'")
case("echo -e 'hello world' | sed 'y/lo/01/'")
case("echo -e 'a/b' | sed 'y,/,|,'")
case("echo -e 'a\\tb' | sed 'y/\\t/ /'")
case("echo -e 'a b' | sed 'y/ /\\n/'")
case("echo -e 'abc' | sed 'y/abc/\\\\xy/'")
case("echo -e 'aXb' | sed 's/x/-/I'")
case("echo -e 'aXb' | sed -n '/x/Ip'")
case("echo -e 'a\\nb' | sed 'N;s/^b/X/M'")
case("echo -e 'a\\nb' | sed 'N;s/a$/X/M'")
case("echo -e 'a\\nb' | sed 'N;s/a$/X/'")
case("echo -e 'a\\nb' | sed 'N;s/^/>/Mg'")
case("echo -e 'a\\nb' | sed 'N;s/$/</Mg'")
case("echo -e 'a\\nb' | sed 'N;l'")
case("echo -e 'a\\tb\\\\c\\001d' | sed -n l")
case("seq 30 | tr -d '\\n' | sed -n l")
case("seq 30 | tr -d '\\n' | sed -n 'l 20'")
case("seq 30 | tr -d '\\n' | sed -n 'l 1'")
case("seq 30 | tr -d '\\n' | sed -n 'l 0'")
case("seq 30 | tr -d '\\n' | sed -l 15 -n l")
case("echo -e 'abc\\ndef' | sed -n 'l;p'")
case("echo -e 'a\\nb\\nc' | sed ':a;N;$!ba;s/\\n/,/g'")
case("echo -e 'a\\nb\\nc' | sed -e :a -e '$!N;s/\\n/,/;ta'")
case("echo -e 'a\\nb\\nc' | sed -n '2{b};p'")
case("echo -e 'a\\nb\\nc' | sed -n '2b end;p;:end'")
case("echo -e 'a\\nb\\nc' | sed '2b;s/./X/'")
case("echo -e 'a\\nb\\nc' | sed 's/a/X/;T;s/X/Y/'")
case("echo -e 'a\\nb\\nc' | sed 's/a/X/;t;s/./Z/'")
case("echo -e 'a\\nb\\nc' | sed 's/a/X/;n;t;s/./Z/'")
case("echo -e 'a\\nb\\nc' | sed 'b end;d;:end'")
case("echo -e 'aaa' | sed ':a;s/a/b/;ta'")
case("echo -e 'a1b22c333' | sed -E ':a;s/([0-9])([0-9])/\\1,\\2/;ta'")
case("echo -e 'abc' | sed -E 's/(.)/\\1\\n/g'")
case("echo -e 'one two' | sed -E 's/(\\w+) (\\w+)/\\2 \\1/'")
case("echo -e 'one two' | sed -E 's/\\s+/\\t/'")
case("echo -e 'one two' | sed 's/\\S\\+/X/g'")
case("echo -e 'one two' | sed 's/\\W/_/'")
case("echo -e 'one two' | sed -n '/one/{s//1/;p}'")
case("echo -e 'one two' | sed -n '/o/{s//0/g;p}'")
case("echo -e 'one two' | sed 's/o/&&&/g'")
case("echo -e 'a.b' | sed 's/./X/g'")
case("echo -e 'a.b' | sed 's/\\./X/g'")
case("echo -e 'a.b' | sed 's/[.]/X/g'")
case("echo -e 'a]b' | sed 's/[]]/X/g'")
case("echo -e 'a-b' | sed 's/[a-]/X/g'")
case("echo -e 'a+b' | sed 's/a+b/X/'")
case("echo -e 'a+b' | sed 's/a\\+b/X/'")
case("echo -e 'a+b' | sed -E 's/a\\+b/X/'")
case("echo -e 'a|b' | sed 's/a|b/X/'")
case("echo -e 'a{1}b' | sed 's/a{1}b/X/'")
case("echo -e 'ab' | sed -E 's/a{1}b/X/'")
case("echo -e 'a*b' | sed 's/*b/X/'")
case("echo -e 'a*b' | sed 's/\\*/X/'")
case("echo -e 'abc' | sed 's/b*/X/2'")
case("echo -e 'abc' | sed 's/b*/X/g'")
case("echo -e 'abc' | sed 's/\\(b\\)*/X/g'")
case("echo -e 'baaac' | sed 's/a*/x/g'")
case("echo -e 'baaac' | sed 's/a*/x/2'")
case("echo -e 'baaac' | sed 's/a*/x/3'")
case("echo -e 'baaac' | sed 's/a*/x/4'")
case("echo -e '' | sed 's/^/x/'")
case("echo -e '' | sed 's/$/x/g'")
case("echo -e '' | sed 's/x*/y/g'")
case("echo a | sed 's/a/b/;t x;s/b/Z/;:x'")
case("echo -e 'a\\nb\\c' | sed p", "echo -e 'a\\nb\\c' | sed -n p")
case("echo -e 'a\\nb\\c' | sed '$!d'")
case("echo -e 'a\\nb\\c' | sed 'a X'")
case("echo -e 'a\\nb\\c' | sed '$a X'")
case("echo -e 'a\\nb\\c' | sed '$i X'")
case("echo -e 'a\\nb\\c' | sed G")
case("echo -e 'a\\nb\\c' | sed -n '$='")
case("echo -e 'a\\nb\\c' | sed 'N;P;D'")
case("echo -e '\\c' > e0", "echo -e 'a\\nb\\c' | sed '$r e0'")
case("echo -e 'a\\nb' > f1", "echo -e 'c\\c' > f2", "sed p f1 f2")
case("echo -e 'a\\c' > f1", "echo -e 'c\\nd' > f2", "sed p f1 f2")
case("echo -e 'a\\c' > f1", "echo -e 'c\\c' > f2", "sed -n p f1 f2")
case("echo -e 'a\\c' > f1", "sed -i s/a/b/ f1", "cat f1")
case("echo -e 'a\\c' > f1", "sed -i 'p' f1", "cat f1")
case("echo -e 'a\\nb' | sed 'x;G'")
case("echo -e 'a\\nb\\nc' | sed -n 'x;p'")
case("echo -e 'a\\nb\\nc' | sed -n 'H;${x;s/\\n/,/g;p}'")
case("echo -e 'a\\nb\\nc' | sed -n '1!G;h;$p'")
case("echo -e 'a\\nb\\nc' | sed 'N;N;D'")
case("echo -e 'a\\nb\\nc' | sed 'N;D'")
case("echo -e 'a\\nb\\nc' | sed '2{h;d};$G'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '2,3{p;n}'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2,3{N;d}'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2,3c X'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2,3!c X'")
case("echo -e 'a\\nb\\nc\\nd' | sed '$!c X'")
case("echo -e 'a\\nb\\nc\\nd' | sed '/b/,/c/{/c/!d}'")
case("echo -e 'a\\nb\\nc\\nd' | sed '/b/,+1d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '/b/,~2d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2,~2d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '3,~2d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '0~2d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2~0d'")
case("echo -e 'a\\nb\\nc\\nd' | sed '2,2p'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '2,1p'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '/b/,1p'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '/b/,/b/p'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '/a/,/b/p;/c/,/d/p'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '2!{3!p}'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '2,3 ! p'")
case("echo -e 'a\\nb\\nc\\nd' | sed '$!{N;N;s/\\n/ /g}'")
case("echo -e 'a\\nb\\nc\\nd' | sed = | sed 'N;s/\\n/ /'")
case("echo -e 'a\\nb\\nc\\nd' | sed -n '$!{n;p}'")
case("echo -e 'a\\nb\\nc\\nd' | sed 'n;n;s/./X/'")
case("echo -e 'a\\nb\\nc' | sed '$!N;s/\\n/-/'")
case("echo -e 'a\\nb\\nc' | sed -n '$!N;p'")
case("echo -e 'a\\nb\\nc' | sed --posix '$!N;s/\\n/-/'")
case("echo -e 'a\\nb\\nc' | sed N")
case("echo -e 'a\\nb\\nc' | sed -n N")
case("echo -e 'a\\nb\\nc' | sed -n 'N;p'")
case("echo -e 'a\\nb\\nc' | sed 'N;N;N;s/\\n/+/g'")
case("echo -e 'a\\nb\\nc' | sed n")
case("echo -e 'a\\nb\\nc' | sed -n 'n;p'")
case("echo -e 'a\\nb\\nc' | sed 'n;n;n;s/./X/'")
case("echo -e 'a\\nb\\nc' | sed '2q'")
case("echo -e 'a\\nb\\nc' | sed '2{p;q}'")
case("echo -e 'a\\nb\\nc' | sed -n '2{p;Q}'")
case("echo -e 'a\\nb\\nc' | sed -e '2{a X' -e 'q}'")
case("echo -e 'a\\nb\\nc' | sed -e '2{a X' -e 'Q}'")
case("echo -e 'a\\nb\\nc' | sed -e '2{a X' -e 'n}'")
case("echo -e 'a\\nb\\nc' | sed -e '2{a X' -e 'N}'")
case("echo -e 'a\\nb\\nc' | sed -e '2{i X' -e 'd}'")
case("echo -e 'a\\nb\\nc' | sed 's/b/X/w /dev/stdout'")
case("echo -e 'a\\nb\\nc' | sed -n 's/b/X/w /dev/stdout'")
case("echo -e 'a\\nb\\nc' | sed -n '2w /dev/stdout'")
case("echo -e 'a\\nb\\nc' | sed -n -e '2w o1' -e '3w o1'", "cat o1")
case("echo -e 'a\\nb\\nc' | sed -n 'w o1'", "cat o1")
case("echo -e 'a\\nb\\nc' | sed 'w o1'", "cat o1")
case("echo -e 'a\\nb\\nc' | sed 'N;w o1'", "cat o1")
case("echo -e 'a\\nb\\nc' | sed -e 's/a/X/' -e 's/X/Y/'")
case("echo -e 'a\\nb\\nc' | sed -n '$ p'")
case("echo -e 'a\\nb\\nc' | sed -n '2,$p'")
case("echo -e 'a\\nb\\nc' | sed '1d;;3d'")
case("echo -e 'a\\nb\\nc' | sed '1d ; 3d'")
case("echo -e 'a\\nb\\nc' | sed '1{d} ; 3d'")
case("echo -e 'a\\nb\\nc' | sed '{1d};{3d}'")
case("echo -e 'a\\nb\\nc' | sed '{{1d}}'")
case("echo -e 'a\\nb\\nc' | sed '1,2{1d}'")
case("echo -e 'a\\nb\\nc' | sed '1,2{2d}'")
case("echo -e 'a\\nb\\nc' | sed '1,3{/b/!d}'")
case("echo -e 'a\\nb\\nc' | sed '1,3!{d}'")
case("echo -e 'a\\nb\\nc' | sed -s 's/a/b/' -")
case("echo -e 'a\\nb\\nc' | sed --separate s/a/b/")
case("echo -e 'a\\nb\\nc' | sed --regexp-extended 's/(a)/\\1\\1/'")
case("echo -e 'a\\nb\\nc' | sed -nE 's/(b)/\\1\\1/p'")
case("echo -e 'a\\nb\\nc' | sed -En 's/(b)/\\1\\1/p'")
case("echo -e 'a\\nb\\nc' | sed -u p")
case("echo -e 'a\\nb\\nc' | sed --unbuffered -n p")
case("echo -e 'a\\nb\\nc' | sed -- p")
case("echo -e 'a\\nb\\nc' | sed -n p -")
case("echo -e 'a\\nb\\nc' | sed -n -e p --")
case("echo -e 'a b' | sed 's/\\(a\\) \\(b\\)/\\2\\n\\1/'")
case("echo -e 'a b' | sed 's/a/\\\\/'")
case("echo -e 'a b' | sed 's/a/\\\\n/'")
case("echo -e 'a b' | sed 's/a/\\&/'")
case("echo -e 'a b' | sed 's/a/\\0\\0/'")

# ---------------------------------------------------------------- sed errors (stderr is not captured: only the status is compared)
for sc in ["k", "s/a/b", "s/a", "s/a/b/q", "s/a/b/gg", "s/a/b/pp", "s/a/b/0", "y/ab/c/", "y/a/b", "{p", "p}", "}", "2,", ",2p",
           "bx", ":", "a", "i", "r", "w", "1,2a x", "1,2i x", "1,2q", "1,2r x", "1,2=", "0p", "0,2p", "/a", "\\", "s/\\(/x/", "s/\\)/x/",
           "s/a\\{1/x/", "s/[a/x/", "s/\\(a\\)/\\2/", "s/a/\\1/", "u", "d x", "p x", "q x", "l x", "= x", "dp", "pp", "}p", "1~", "~2p", "+1p"]:
    ERRS.append(sc)

# ---------------------------------------------------------------- shell wildcards (lowercase names; the guest's root also holds other files, so every pattern starts with zq)
G = ["echo 1 > zqa.txt", "echo 2 > zqb.txt", "echo 3 > zqc.dat", "echo 4 > zq10.txt", "echo 5 > .zqhid", "mkdir zqd", "echo 6 > zqd/in.txt", "echo 7 > zqd/zqx.dat"]
for pat in ["zq*", "zq*.txt", "zq?.txt", "zq??.txt", "zq[ab].txt", "zq[a-c].*", "zq[!a].*", "zq[^a].*", "zqd/*", "zqd/*.txt", "zq*/in.txt",
            "zq*/*", "zq*d/z*", ".zq*", "zq*x", "zq[", "zq[a", "zq[]", "zqa.t?t", "zq?*", "*zq*", "zqnone*", "zqd/none*", "nodir/*", "zq[a-]*", "zq*.*"]:
    case(*G, f"echo {pat}")
    case(*G, f"echo x {pat} y")
case(*G, "echo 'zq*' \"zq?\"")
case(*G, "echo zq*.txt zqd/*.txt")
case(*G, "echo zq*.txt | wc -w")
case(*G, "cat zq?.txt")
case(*G, "cat zq[ab].txt zq10.txt | sort")
case(*G, "head -n 1 zq*.txt")
case(*G, "cat zqd/*.txt zq[ab].txt > zqout", "cat zqout")
case(*G, "grep -c . zq*.txt")
case(*G, "?cmp zqa.txt zqb.txt")
case(*G, "echo \"a b\"*", "echo zq\"*\"")
case(*G, "echo zq*.txt > zqlist", "cat zqlist")
case(*[f"echo a > zqm{i}" for i in range(1, 21)], "echo zqm*", "echo zqm* | wc -w", "head -n 1 zqm*")

# ---------------------------------------------------------------- diff
D = ["echo -e '1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n10' > da",
     "echo -e '1\\n2\\nthree\\n4\\n5\\n6\\n7\\n9\\n10\\n11' > db",
     "echo -e 'Hello  World\\nfoo\\n bar \\nsame' > dc",
     "echo -e 'hello world\\nfoo\\nbar\\nsame' > dd",
     "echo -e 'a\\nb\\na\\nb\\na' > de",
     "echo -e 'b\\na\\nb\\nc' > df"]
L = "--label A --label B"
for args in ["da db", "db da", "da da", "de df", "df de", "dc dd", "da dc", f"-u {L} da db", f"-U0 {L} da db", f"-U1 {L} db da",
             f"-U 5 {L} da db", f"--unified=2 {L} de df", f"-c {L} da db", f"-C1 {L} de df", f"-C 0 {L} da db", f"--context {L} df de",
             "-q da db", "-q da da", "-s da da", "-s da db", "--brief de df", "-i dc dd", "-b dc dd", "-w dc dd", "-i -w dc dd",
             "-ib dc dd", f"-u -w {L} dc dd", "--ignore-case dc dd", "--ignore-all-space dc dd", "-d de df", "--minimal da db",
             f"-u -U1 {L} da db", f"-U1 -u {L} da db", f"-c -C1 {L} da db"]:
    case(*D, f"?diff {args}")
# no newline at the end, and empty files
case(*D, "echo -n 10 > dn", "?diff da dn", "?diff dn da", f"?diff -u {L} da dn", f"?diff -c {L} dn da", "?diff -b da dn", "?diff dn dn")
case(*D, "echo -n > dz", "?diff dz da", "?diff da dz", f"?diff -u {L} dz da", f"?diff -c {L} da dz", "?diff dz dz")
# standard input
case(*D, "?cat da | diff - db", f"?cat db | diff -u {L} da -", "cat da | diff -s - da")
# longer files
case("seq 1 200 > s1", "seq 1 200 | sed -e s/7/seven/ -e /^15/d > s2", "?diff s1 s2", f"?diff -u {L} s1 s2", f"?diff -C2 {L} s2 s1")
case("seq 1 50 > s1", "seq 3 60 > s2", "?diff s1 s2", f"?diff -u {L} s2 s1")
# directories
T = D + ["mkdir x", "mkdir y", "mkdir x/sub", "mkdir y/sub", "mkdir x/xonly", "cp da x", "cp db y/da", "cp de x/same", "cp de y/same",
         "cp df x/sub/f", "cp de y/sub/f", "cp da x/gone", "cp db y/new", "cp da x/sub/deep", "mkdir y/kind", "cp da x/kind"]
for args in ["x y", "y x", "-r x y", "-q x y", "-rq x y", "-rs x y", "-N x y", "-rN x y", "-rqN y x", f"-r -u {L} x y", "-r x/sub y/sub",
             "da y", "x/same y", "-r -c --label A --label B x y"]:
    case(*T, f"?diff {args}")

# binary files, and a name GNU quotes
case("echo -e 'a\\0b' > b1", "echo -e 'a\\0c' > b2", "?diff b1 b2", "?diff -q b1 b2", "diff b1 b1", "diff -s b1 b1", "cp b1 b3", "diff b1 b3")
case("mkdir x", "mkdir y", "echo 1 > \"x/s p\"", "echo 2 > \"y/s p\"", "?diff x y", "?diff -q x y", "?diff \"x/s p\" y")

# ----------------------------------------------------------------- tac
# `echo -e '...\c'` writes exactly what is inside, with no newline of its own.
def raw(text):
    return f"echo -e '{text}\\c' | "

for inp in [r"a\nb\nc\n", r"a\nb\nc", r"\n\na\n\n", r"x\n", "x", r"one\n\ntwo\n\n\n", r"\n", r"\n\n", r"a\n\nb"]:
    case(raw(inp) + "tac")
case("echo -n | tac")
for sp in [",", "XX", "ab", "'\\n'", "XYZ", "abab"]:
    for inp in ["a,b,c", "a,b,c,", ",a,b,c", "a,,b", ",", ",,", "XXXX", "aXXXb", "XXaXXbXX", "aXYZbXYZc", "abababab", "xabcabyab", "x"]:
        case(raw(inp) + f"tac -s {sp}")
        case(raw(inp) + f"tac -b -s {sp}")
case(raw("a,b,c") + "tac -s, ")
case(raw("a,b,c") + "tac -bs,")
case(raw("a,b,c") + "tac -sb,")
case(raw("a,b,c") + "tac -s ,")
case(raw("a,b,c,") + "tac -b -s , -b")
case(raw("a,b,c") + "tac -s , -s ';'")
case(raw("a;b,c") + "tac -s , -s ';'")
# an empty separator is a NUL byte (tr makes the NULs visible)
case(raw(r"a\0b\0c\0") + "tac -s '' | tr '\\000' '/'")
case(raw(r"a\0b\0c") + "tac -s '' | tr '\\000' '/'")
case(raw(r"a\0b\0c\0") + "tac -b -s '' | tr '\\000' '/'")
case(raw(r"a\0b\nc\n") + "tac | tr '\\000' '/'")
# files and standard input
case("echo -e '1\\n2\\n\\c' > f1", "echo -e '3\\n4\\c' > f2", "tac f1 f2", "tac f2 f1", "tac f1 f1", "tac f1 - f2 < f1")
case("echo -e '1\\n2\\n\\c' > f1", "echo -e 'x\\ny\\n\\c' | tac - f1", "echo -e 'x\\ny\\n\\c' | tac f1 -")
case("echo -e 'p,q\\n\\c' > f1", "echo -e 'r,s' > f2", "tac -s , f1 f2", "tac -b -s , f1 f2")
case("echo -e 'a\\nb\\n\\c' > f1", "tac -- f1", "tac f1 > f2", "cat f2", "tac f2 | tac")
case("seq 1 5 | tac", "seq 1 2000 | tac | head -n 3", "seq 1 2000 | tac | tail -n 2", "seq 1 5000 | tac | wc -l", "seq 1 3000 | tac | tac | tail -n 1")
case("seq 1 300 | tr '\\n' ' ' | tac -s ' ' | head -c 40")

# ------------------------------------------------------------------ rev
for inp in [r"abc\n", r"abc", r"a\nbc\ndef\n", r"\n\nab\n", r"\n", r"x\n\n", r"ab cd  ef\n", r"a\tb\n", r"x y\nz", "a", r"123456789012345678901234567890\n",
            r"\"quoted\" 'text'\n", r"back\\slash\n", r"a\0b\n"]:
    case(raw(inp) + "rev | tr '\\000' '/'")
case("seq 1 100 | tr '\\n' ' ' | rev")
case("seq 1 400 | tr '\\n' ',' | rev | wc -c", "seq 1 400 | tr '\\n' ',' | rev | tr ',' ' ' | head -c 30")
case("seq 1 40 | rev", "seq 1 40 | rev | rev | head -n 3", "seq 10 20 | rev | sort | head -n 4")
case("echo -e 'ab' > f1", "echo -e 'cd\\nef' > f2", "rev f1 f2", "rev f2 f1", "echo -e 'xy\\c' > f3", "rev f3 f2", "rev f3 f3")
case("echo -e 'abc\\ndef\\n' > f1", "rev f1 | rev | cmp - f1", "rev -- f1")
case("echo -e 'hello world\\n' | rev | rev", "echo hello | rev | tac", "echo -e 'one\\ntwo' | rev | tac")

# ------------------------------------------------------------------- nl
SIMPLE = r"a\n\nb\n  \nc\n"
for inp in [SIMPLE, r"a\nb", r"\n", "", r"x", r"\n\n\n", r"one\n\n\n\ntwo\n\n\nthree\n"]:
    for fl in ["", "-ba", "-bn", "-bt", "-b a", "-b t", "-b n"]:
        case(raw(inp) + f"nl {fl}")
FLAGS = ["-nln", "-nrn", "-nrz", "-n ln", "-n rz", "-nrz -w3", "-nln -w 8", "-w1", "-w 2", "-w 12", "-w+4", "-w ' 4'", "-s:", "-s ' | '", "-s ''", "-s 'ab'",
         "-s '\\t'", "-v5", "-v -3", "-v -3 -nrz", "-v 0", "-v +7", "-v 99999999", "-v 123456 -w3", "-v 12 -nln -w 5 -s '|'", "-i2", "-i -1", "-i0", "-i 10 -v 5", "-i -5 -v 0 -nrz",
         "-ba -nrz -w3 -s: -v10 -i5", "-ba -w 2 -nln -s '--'", "-bn -v 7", "-ba -l2", "-ba -l3", "-ba -l 1", "-ba -l5", "-l2", "-ba -l2 -i3 -v 0",
         "-b 'p^a'", "-bp^a", "-bpa", "-b 'p[a-c]'", "-b 'p^$'", "-b p", "-b 'p.'", "-bp'^[ab]' -nrz", "-b 'px\\|a'", "-b 'pb*'", "-bpc -s:"]
for fl in FLAGS:
    case(raw(SIMPLE) + f"nl {fl}")
# numbers up to the end of what 64 bits hold (a line that would need more is an error: see the status checks)
case(raw(r"a\nb\n") + "nl -v 9223372036854775806", raw(r"a\n\nb\n") + "nl -v 9223372036854775806", raw(r"a\nb\n") + "nl -v 9223372036854775807 -i 0", raw(r"a\nb\nc\n") + "nl -v 9223372036854775800 -i 3")
case(raw(r"a\nb\n") + "nl -v -9223372036854775807 -i -1 -nrz", raw(r"a\n") + "nl -v -9223372036854775808", raw(r"a\nb\nc\n") + "nl -v -9223372036854775800 -i -3 -w 25", raw(r"a\n") + "nl -v 9223372036854775807 -nln -w 30")
case(raw(r"one\n\n\n\ntwo\n\n\nthree\n\n") + "nl -ba -l2")
case(raw(r"one\n\n\n\ntwo\n\n\nthree\n\n") + "nl -ba -l3 -nrz")
case(raw(r"a\n\n\n\n\n\n\nb\n") + "nl -ba -l3")
case(raw(r"a\n\n\n\n\n\n\nb\n") + "nl -ba -l4")
case(raw(r"a\n\n\n\nb\n\n\nc\n") + "nl -l2")
# files and standard input: the numbers go on from one to the next
case("echo -e 'a\\nb\\n\\c' > f1", "echo -e 'c\\n\\nd\\c' > f2", "nl f1 f2", "nl f2 f1", "nl f1 - f2 < f2", "nl -ba f1 f2", "nl -v 10 -s: f1 f2 f1", "nl f1 -w3 f2", "nl -w3 -s: -- f1", "nl - < f1")
case("echo -e 'a\\nb\\n\\c' > f1", "nl f1 > f2", "cat f2", "nl f2", "nl -ba f2")
case("seq 1 12 | nl", "seq 1 12 | nl -w2 -nln -s '.'", "seq 95 105 | nl -w 3 -v 95 -nrz", "seq 1 2500 | nl | tail -n 2", "seq 1 20 | nl -i 7 -w 4 | tail -n 3")
case("seq 1 6 | nl -b 'p[135]'", "seq 1 6 | nl -b 'p[135]' -nln -s '>'", "seq 1 30 | nl -b 'p3'")
# pages: header, body and footer, and the markers that tell them apart
PG = r"top\n\\:\\:\\:\nH1\nH2\n\\:\\:\nb1\n\nb2\n\\:\nf1\nf2\n\\:\\:\\:\nH3\n\\:\\:\nb3\n\\:\\:\nb4\n"
for fl in ["", "-ha", "-fa", "-ha -fa", "-ba", "-ba -ha -fa", "-ha -fa -bn", "-hn -fn -ba", "-p", "-p -ha", "-p -ba -ha -fa", "-ha -v 5", "-ha -p -v 5", "-ha -fa -nrz -w2 -s:",
           "-ht -ft", "-hp^H -fp^f", "-hp1 -fp2 -b 'p[0-9]'", "-ba -l2 -ha -fa", "-ha -fa -i3", "-ha -fa -i3 -p", "-hn -bt -fa -v -2"]:
    case(raw(PG) + f"nl {fl}")
# other markers
for d, markers in [("'@:'", r"@:\n@:@:\n@:@:@:\n"), ("@", r"@:\n@:@:\n@:@:@:\n"), ("'ab'", r"ab\nabab\nababab\n"), ("'abc'", r"abc\nabcabc\nabcabcabc\n"), ("'x'", r"x:\nx:x:\nx:x:x:\n"),
                   ("'#!'", r"#!#!#!\nz\n#!#!\n#!\n"), ("''", r"\\:\n\\:\\:\nz\n")]:
    case(raw(r"p\n" + markers + r"q\nr\n") + f"nl -d {d} -ba -ha -fa")
    case(raw(r"p\n" + markers + r"q\nr\n") + f"nl -d{d} -ba")
case(raw(r"\\:\\:\\:\nx\n\\:\\:\\:\nx\n") + "nl -ha")
case(raw(r"a\n\\:\\:\\:\\:\nb\n\\:\n\\:\\:\\:\n\\:\\:\\:\nc\n") + "nl -ba -ha -fa")
case(raw(r" \\:\\:\\:\nb\n\\:\\:\\: \nc\n\\:\\:\\:\\:\\:\\:\nd\n") + "nl -ba -ha -fa")
case(raw(r"\\:\\:\\:") + "nl -ba -ha")
case(raw(r"x\n\\:\\:\\:") + "nl -ba -ha")
case(raw(r"x\n\\:\\:\\:\ny") + "nl -ba -ha -v 3")
# a page marker starts the numbers again, so it also forgives a number that ran out
case(raw(r"a\n\\:\\:\nb\n") + "nl -v 9223372036854775807", raw(r"a\nb\n") + "nl -v 9223372036854775807 -i -1", raw(r"a\nb\nc\n") + "nl -v 9223372036854775806 -i 0")

# ---------------------------------------------------------------- paste
def mkf(name, text):
    return "echo -e '" + text.replace("\n", "\\n") + "\\c' > " + name

PF = {"p1": "a\nb\nc\n", "p2": "1\n2\n", "p3": "x\ny", "pe": "", "pb": "a\n\nb\n", "pl": "one\ntwo\nthree\nfour\nfive\n"}
def pmk(*names):
    return [mkf(n, PF[n]) for n in names]

SETS = [["p1"], ["p1", "p2"], ["p2", "p1"], ["p1", "p2", "p3"], ["p3", "p1"], ["pe", "p1"], ["p1", "pe"], ["pe", "pe"],
        ["p1", "p1"], ["pb", "p1"], ["p3"], ["pe"], ["p2", "p3", "p1", "pb"]]
POPTS = ["", "-s", "-d,", "-d ,", "-d ',;'", "-d '\\0'", "-d ''", "-d '\\0,'", "-d '\\t\\n'", "-d '\\\\'", "-d 'ab'", "-d '\\q'",
         "-s -d,:", "-sd ,:", "-d ,: -s", "-d '\\n'", "-s -d '\\0'", "-s -d ''", "--", "-s --"]
for names in SETS:
    for o in POPTS:
        case(*pmk(*names), "paste " + o + " " + " ".join(names))
case("echo -e '1\\n2\\n3\\n4\\n5\\c' | paste - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste - - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste - - - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -d: - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -d ':,' - - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -s -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -s - -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -s -d '+' -")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -s")
case("echo -e '1\\n2\\n3\\n4\\n5\\n6\\c' | paste -d '|' -s")
case(mkf("p1", PF["p1"]), "echo -e 'q\\nr\\c' | paste p1 -")
case(mkf("p1", PF["p1"]), "echo -e 'q\\nr\\c' | paste - p1")
case(mkf("p1", PF["p1"]), "echo -e 'q\\nr\\nz\\nw\\c' | paste - p1 -")
case(mkf("p1", PF["p1"]), "echo -e 'q\\nr\\nz\\nw\\c' | paste -s p1 - p1")
case(mkf("p1", PF["p1"]), "?paste p1 nosuch p1")
case(mkf("p1", PF["p1"]), "?paste nosuch p1")
case(mkf("p1", PF["p1"]), "?paste -d 'x\\' p1")
case("seq 1 3000 | paste - - - -")
case("seq 1 3000 | paste -s -d ',' | wc -c")
case("seq 1 5000 > pa", "seq 5001 9000 > pb", "paste pa pb | tail -n 3")
case("seq 1 5000 > pa", "seq 5001 9000 > pb", "paste pb pa | tail -n 3")
case("seq 1 3 > pa", "echo -e 'a\\n\\nb\\c' > pb", "paste -d '\\0' pa pb pa")

# ---------------------------------------------------------------- comm
CF = {"A": "a\nb\nc\nd\n", "B": "b\nc\nx\ny\n", "E": "", "N": "a\nb", "D1": "a\na\nb\n", "D2": "a\nb\nb\n",
      "U": "b\na\nc\n", "U2": "a\nc\nb\nd\n", "S": "a\nb\nc\nd\n", "X": "b\na\n", "Y": "c\n", "L": "line 1\nline 10\nline 2\n", "M": "line 1\nline 2\n", "Z": "a\nb\nb\n"}
CPAIRS = [("A", "B"), ("B", "A"), ("A", "A"), ("A", "E"), ("E", "B"), ("E", "E"), ("D1", "D2"), ("N", "B"), ("A", "N"),
          ("U", "A"), ("A", "U"), ("U2", "S"), ("S", "U2"), ("U2", "U2"), ("X", "Y"), ("Y", "X"), ("L", "M"), ("Z", "D2")]
COPTS = ["", "-1", "-2", "-3", "-12", "-13", "-23", "-123", "-1 -3", "-3 -2", "--total", "--total -12", "--total -123",
         "--output-delimiter=:", "--output-delimiter=ab -3", "--output-delimiter= -1", "--output-delimiter=: -2 --total",
         "--check-order", "--nocheck-order", "--check-order --nocheck-order", "--nocheck-order --check-order",
         "--out=: --tot", "--no", "--check", "--", "--check-order -1", "--check-order --total"]
for a, b in CPAIRS:
    for o in COPTS:
        case(mkf("ca", CF[a]), mkf("cb", CF[b]), "?comm " + o + " ca cb")
case(mkf("ca", CF["A"]), "?echo -e 'c\\nz\\c' | comm ca -")
case(mkf("ca", CF["A"]), "?echo -e 'c\\nz\\c' | comm - ca")
case(mkf("ca", CF["A"]), "?echo -e 'c\\nz\\c' | comm -12 - ca")
case(mkf("ca", CF["A"]), "?comm ca nosuch")
case(mkf("ca", CF["A"]), "?comm nosuch ca")
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm ca cb --total -3")
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm -12 -- ca cb")
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm -- ca cb")
case("seq 1 3000 > ca", "seq 1 2 3000 > cb", "comm -23 ca cb | tail -n 2", "comm -12 ca cb | wc -l")
case("seq 1 3000 > ca", "seq 1 2 3000 > cb", "?comm ca cb | wc -l")
case("echo -e 'a\\nb\\nc' > ca", "echo -e 'b' > cb", "echo -e 'b\\nb' > cc", "?comm ca cb", "?comm ca cc", "?comm cc ca")

# high bytes sort after ASCII, which only an unsigned comparison gives (the
# screen cannot show them, so tr turns them into '+' on the way)
HB = "| tr '\\200-\\377' '+'"
def hb(name, text):
    return "echo -e '" + text + "\\c' > " + name
case(hb("ca", "a\\n\\0300\\n"), hb("cb", "b\\n\\0200\\n"), "comm ca cb " + HB)
case(hb("ca", "a\\n\\0300\\n"), hb("cb", "b\\n\\0200\\n"), "?comm --check-order ca cb " + HB)
case(hb("ca", "a\\n\\0377\\n"), hb("cb", "a\\n\\0200\\n\\0377\\n"), "comm --check-order ca cb " + HB, "comm -12 ca cb " + HB)
case(hb("ca", "a\\n\\0300\\n\\0377\\n"), hb("cb", "\\0300\\n\\0301\\n"), "comm -3 ca cb " + HB, "comm -12 ca cb " + HB)
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm --output-delimiter=a --output-delimiter=b ca cb")
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm --output-delimiter=ab --output-delimiter=ab ca cb")
case(mkf("ca", CF["A"]), mkf("cb", CF["B"]), "?comm --output-delimiter=ab --output-delimiter=ac ca cb")
case(mkf("ca", "ab\n\n"), mkf("cb", "ab\n"), "?comm ca cb")
case(mkf("ca", "\n"), mkf("cb", "\n\n"), "?comm ca cb")
case(mkf("ca", "a b\na\n"), mkf("cb", "a\na b\n"), "?comm ca cb")
case(hb("pa", "a\\0200b\\n"), hb("pb", "\\0200\\n"), "paste -d '\\0200' pa pb " + HB)

# where GNU checks the order: as a line is read, and again at the end of a file
for a, b in [("b\na\nd\n", "b\ne\n"), ("b\na\n", "b\nc\nd\n"), ("b\na\nc\n", "b\nc\n"), ("b\n", "b\na\n"), ("b\n", "b\na\nc\n"),
             ("b\na\n", "b\nb\n"), ("b\na\n", "b\n0\n"), ("x\nb\na\n", "x\n"), ("x\nb\na\n", "x\nz\n"), ("x\nb\na\n", "x\nc\n"),
             ("a\nb\nb\na\n", "a\nb\nb\na\n"), ("a\nb\na\nz\n", "a\nb\nz\n"), ("b\na\n", "b\nc\n"), ("b\na", "b\nc")]:
    for o in ["", "-12", "--check-order", "--nocheck-order", "--total"]:
        case(mkf("ca", a), mkf("cb", b), "?comm " + o + " ca cb")

# ---------------------------------------------------------------- expand, unexpand, fold
# Tabs, backspaces and carriage returns would move the cursor on the screen
# (and the blanks are hard to count), so they are shown as T, B, R and '.'.
VIS = "| tr '\\011\\010\\015 ' 'TBR.'"
def tf(name, text):
    for ch, esc in (("\\", "\\\\"), ("\n", "\\n"), ("\t", "\\t"), ("\b", "\\b"), ("\r", "\\r")):
        text = text.replace(ch, esc)
    return "echo -e '" + text + "\\c' > " + name

XT = ["a\tb\tc\n", "\t\tx\n", "ab\bc\td\n", "\b\bx\ty\n", "abc\rd\te\n", "      \tx\n", "x\t", "\n\n\t\n", "a\t\nb\n", "",
      "abcdefgh\tx\n", "abcdefghi\tx\n", "\t \t x \t\n", "ab\t\bc\n"]
XO = ["", "-t 4", "-t4", "-t 3,10", "-t 3 10", "-t 2,/5", "-t 2,+5", "-i", "-i -t 4", "--tabs=5", "--tabs 5", "--initial",
      "-it 3", "-t 1", "--ini -t 2", "-t 4,8,12", "-t 5 -t 7", "-t /4", "-t +4", "-t 2,5,/3", "-t '3, 6'", "-- -i"]
for i, t in enumerate(XT):
    for j, o in enumerate(XO):
        if (i + j) % 3 == 0 or o in ("", "-t 4", "-i"):
            case(tf("xf", t), "expand " + o + " xf " + VIS)
for t in XT[:6]:
    case(tf("xf", t), "cat xf | expand " + VIS, "cat xf | expand -t 3 - " + VIS, "expand - < xf " + VIS)
XBAD = ["-t 0", "-t 3,2", "-t 3,3", "-t ,", "-t x", "-t 4,/8,12", "-t '/4,+8'", "-t 4/", "-t 4+", "-t 3,x", "-t -3",
        "-t 1/2,4", "-t '/2,3/4'", "-t 1/2,4,5", "-t +0,3+4,5", "-t '4+ 5'", "-t 11/,8", "-t +3,+3", "-t /4,/4", "-t 3,+3,3",
        "-t +99999999999999999999,3,4", "-t '/99999999999999999999 3 4'", "-t 5,99999999999999999999,3,4", "-t 18446744073709551616",
        "-t 0,3", "-t '4 ,8 x'", "-t 2,+0,3", "-t 2,/4/", "-t +3,+4", "-t /3 -t 2", "-t 5 -t 3", "-t 7,/", "-t 7,+", "-t 1,2,+"]
for o in XBAD:
    case("echo a > xf", "?expand " + o + " xf")
case(tf("xa", "a\t"), tf("xb", "b\tc\n"), "expand xa xb " + VIS, "expand -t 4 xa xb xa xb " + VIS, "expand xa - xb < xb " + VIS)
case(tf("xa", "a\tb\n"), "?expand xa nosuch xa > xo", "cat xo " + VIS, "?expand nosuch", "?expand -t 3 xa nosuch > xo", "cat xo " + VIS)
case("mkdir xd", "?expand xd", "?expand . ")
case("seq 1 3000 | paste - - - > xl", "expand xl | wc -c", "expand xl | tail -n 2 " + VIS, "expand -t 3,10 xl | tail -n 2 " + VIS,
     "expand -t 5 xl | wc -c", "expand -i xl | wc -c")
case("seq 1 20000 | paste -s -d '\\t' > xl", "expand xl | wc -c", "expand -t 3 xl | wc -c", "expand -t 1000 xl | wc -c")

UT = ["        x\n", "    x\n", "a       b\n", "a b\n", "  \t x\n", "x        y\n", "a         b\n", "\t\t x\n", "        \n", "         \n",
      "a  b   c    d     e      f       g        h\n", "       x\n", "a       \tb\n", " \n", "        ", "x\t       y\n", "ab\b       c\n",
      "  a   b\n", "                x\n", "a\n        b\n", "\ta    b\n", "ab  cd  ef\n", "x       \n", "x            y\n", "\b        x\n", "\b\bx   y\n", "a\n x\n  y\n", "\n x\n"]
UO = ["", "-a", "--first-only", "-a --first-only", "-t 4", "-t 4,8", "-t 2,/4", "-a -t 3", "--all", "-t 3,+6", "-t 3 -a", "-t 5", "--first-only -t 3",
      "-t 1", "-t 2,4,6", "--first -a", "-- -a"]
for i, t in enumerate(UT):
    for j, o in enumerate(UO):
        if (i + j) % 3 == 0 or o in ("", "-a", "-t 4"):
            case(tf("uf", t), "unexpand " + o + " uf " + VIS)
for t in UT[-4:]:
    for o in ["-t 1", "-t 1,3", "-a", "-t 2 -a", ""]:
        case(tf("uf", t), "unexpand " + o + " uf " + VIS)
for t in UT[:8]:
    case(tf("uf", t), "cat uf | unexpand -a " + VIS, "unexpand - < uf " + VIS)
UBAD = ["-t 0", "-t 3,2", "-t x", "-t 4,/8,12", "-t '/4,+8'", "-t 4/"]
for o in UBAD:
    case("echo a > uf", "?unexpand " + o + " uf")
case(tf("ua", "a      "), tf("ub", " b\n"), "unexpand -a ua ub " + VIS, "unexpand ua ub " + VIS, "unexpand -a ua - ub < ub " + VIS)
case(tf("ua", "a        b\n"), "?unexpand -a ua nosuch ua > uo", "cat uo " + VIS, "?unexpand nosuch")
case("seq 1 3000 | paste - - - | expand > ul", "unexpand -a ul | wc -c", "unexpand -a ul | tail -n 2 " + VIS, "unexpand ul | wc -c",
     "unexpand -t 3,10 ul | tail -n 2 " + VIS)
case("seq 1 4000 | sed 's/^/        /' > ul", "unexpand ul | wc -c", "unexpand ul | tail -n 2 " + VIS, "unexpand -t 4 ul | wc -c")
case("seq 1 20000 | paste -s -d ' ' | sed 's/ /       /g' > ul", "unexpand -a ul | wc -c", "unexpand -a -t 5 ul | wc -c")

FT = ["abcdefghijklmnopqrstuvwxyz\n", "the quick brown fox jumps over the lazy dog\n", "the quick brown fox jumps over the lazy dog", "\n\n",
      "a\tb\tc\td\te\n", "a\tb c\td\n", "ab\bcd\bef\bgh\n", "abc\rdefghijklmnop\n", "0123456789012345678901234567890123456789\n",
      "aaaaaaaaaa bbbbbbbbbb cccccccccc\n", "   leading and trailing   \n", "x y z w v u t s r q p o n m l k\n", "a  b  c  d  e  f  g  h\n",
      "\t\t\t\tdeep\n", "aaaa\n\nbbbbbbbbbbbb\n", "", "word\t\tword\tword\n", "abcdefghij klmnopqrst uvwxyz0123 456789\n", "a\bb\bc\bd\be\bf\bg\bh\bi\bj\bk\n", "a\b\bb\n", "\b\bab\n", "x\b\b\bab\n"]
FO = ["", "-w 10", "-w10", "-10", "-s -w 10", "-b -w 8", "-bs -w 5", "--width=7", "--width 7 -s", "--spaces -w 6", "--bytes -w 4", "-w 1", "-w 3 -s",
      "-sw5", "-w 20", "-s -w 20", "-b", "-s", "-1", "-w 8 -w 4", "-b -w 12 -s", "-w +6", "-s -w 2", "--wid=9", "-- -w5"]
for i, t in enumerate(FT):
    for j, o in enumerate(FO):
        if (i + j) % 3 == 0 or o in ("", "-w 10", "-s -w 10"):
            case(tf("ff", t), "fold " + o + " ff " + VIS)
for t in FT[-3:]:
    for o in ["-w 2", "-b -w 2", "-s -w 3", "-w 80"]:
        case(tf("ff", t), "fold " + o + " ff " + VIS)
for t in FT[:6]:
    case(tf("ff", t), "cat ff | fold -w 7 " + VIS, "fold -s -w 7 - < ff " + VIS)
FBAD = ["-w 0", "-w x", "-w ''", "-w 5x", "-5s", "-w -5", "-w 18446744073709551615", "-w 18446744073709551616", "-w 99999999999999999999999", "-w ' 5'", "-0"]
for o in FBAD:
    case("echo abcdefghij > ff", "?fold " + o + " ff")
case("echo abc > ff", "?fold -w 18446744073709551606 ff", "?fold -w 18446744073709551607 ff")
case("echo abcdefghij > ff", "?fold -w 18446744073709551610 ff", "?fold -w 000000000000000000000000000004 ff", "fold -w +4 ff")
case(tf("fa", "abcdef"), tf("fb", "ghijkl\n"), "fold -w 4 fa fb " + VIS, "fold -w 4 fb fa fb " + VIS, "fold -w 4 fa - fb < fb " + VIS)
case(tf("fa", "abcdef\n"), "?fold -w 4 fa nosuch fa > fo", "cat fo " + VIS, "?fold -w 4 nosuch")
case("mkdir fd", "?fold fd", "?fold . ")
case("seq 1 5000 | paste -s -d ' ' > fl", "fold -s -w 70 fl | tail -n 3", "fold -s -w 70 fl | wc -l", "fold -w 1000 fl | wc -l", "fold -w 1 fl | wc -l",
     "fold -b -w 33 fl | tail -n 2")
case("seq 1 20000 | paste -s -d x > fl", "fold -w 50 fl | wc -l", "fold -s -w 50 fl | wc -l", "fold -w 100000 fl | wc -l", "fold -w 3 fl | tail -n 2")
case("seq 1 20000 | paste -s -d ' ' > fl", "fold -s -w 100 fl | wc -l", "fold -s -w 100 fl | tail -n 2", "fold -w 100 fl | tail -n 1")
case("seq 1 3000 | paste - - - - > fl", "fold -w 8 fl | wc -l", "fold -w 8 -s fl | tail -n 2 " + VIS, "fold -b -w 8 fl | wc -l")

# ---------------------------------------------------------------- od, strings
# Files with every kind of byte, written with echo -e (\xHH always has two
# digits, so the text after it is never taken for part of the escape).
def binf(name, data):
    out = ""
    for b in data:
        c = chr(b)
        if c == "\n": out += "\\n"
        elif c == "\\": out += "\\\\"
        elif 32 <= b < 127 and c != "'": out += c
        else: out += "\\x%02x" % b
    return "echo -e '" + out + "\\c' > " + name

BF = binf("bf", b"hello\n\x00\x01\xff\x80\x7f\x1bABCDEFGHIJKLMNOPQRSTUVWXYZ")
RF = "yes ABCDEFGHIJKLMNO | head -n 5 > rg"      # five equal lines of 16 bytes
ZF = "yes | head -n 40 | tr 'y\\n' '\\000\\000' > zf"   # 80 zero bytes
OD_BF = [
    "", "-c", "-a", "-b", "-d", "-h", "-i", "-l", "-o", "-s", "-x", "-A x", "-A d", "-A n", "-An -c", "-A o -t x1", "-A xx",
    "-t x1", "-t x1z", "-t x2", "-t x4", "-t x8", "-t xz", "-t x2z", "-t o1", "-t o2", "-t o4", "-t o8", "-t u1", "-t u2",
    "-t u4", "-t u8", "-t d1", "-t d2", "-t d4", "-t d8", "-t a", "-t c", "-t cz", "-t az", "-t d", "-t o", "-t u", "-t xC",
    "-t xS", "-t xI", "-t xL", "-t dC -t dS", "-t uI", "-t oL", "-t x1x1", "-t c -t a", "-t d1 -t u1", "-t x1 -t x8",
    "-t x2z -t c", "-t x4z", "-t o8 -t u8 -t d8", "-t xC -t dS -t uI -t oL", "-t x1z -t d2", "-t x1z -t d2z", "-t cz -t x2z",
    "-t x1 -t x2 -t x4 -t x8", "-t d1 -t d2 -t d4 -t d8", "-t a -t c -t o1 -t u1", "-t a -t d2 -t x4", "-t d8 -t c",
    "--format=x1", "--format x1", "--format=x1z", "--format=c", "-c -a -b", "-x -o", "-vc", "-v", "--output-duplicates", "--out -c",
    "-t ''", "-t x1 -t ''", "-t x1x1x1x1", "-t o1d2u4x8",
    "-w -t x1", "-w8 -t x4", "-w3 -t x4", "-w5 -t x1", "-w7 -t x1", "-w1 -t x1", "-w2 -t x1", "-w4 -t x1 -t d2 -t c",
    "-w12 -t x1 -t d4 -t c", "-w24 -t x1 -t d8 -t c", "-w6 -t x1 -t d2 -t c", "-w -c", "-w32 -t x1z", "-w8 -c", "-w9 -c",
    "--width=4 -t x1", "--width -t x1", "--width=12 -t c", "-w+8 -t x1", "-cw8", "-w1 -w2 -t x1", "-w3 -t d8",
    "-j 3", "-j 3 -N 2", "-j 37", "-j 38", "-j 0x10 -N 0x4", "-j 010 -N 010", "-j 1 -t x1", "-N 3", "-N 3 -t x4", "-N 0",
    "-N 1 -t x2", "-N 5 -t x8", "-N 20 -A d", "-N 0x10", "-N 1kB", "-N 2b", "-N ' 3'", "-N +3", "-N2 -N3", "-j 5 -A x -t c",
    "--skip-bytes=3 --read-bytes=2", "--skip-bytes 3 --read-bytes 2", "--address-radix=d -N2", "--a=x -N 20", "--read=2",
    "-- ", "-c -- ", "-N 1m", "-N 1M -c", "-N 1G -c", "-N 15E -c", "-N 1 -N 4k", "-An -t x1 -t c", "-An -c -a", "-An -t x1z -t d2", "-j 1 -- ", "-N 18446744073709551615 -c", "-N 18446744073709551KB -c", "-w65536 -t x1", "-j 1 -N 1 -t x1", "-j 36 -t c", "-j 2 -N 3 -t c -A d", "-N 1 -t d8", "-N 7 -t x2z", "-N 9 -t x4 -t c", "-N 21 -t d2 -t x8",
]
for o in OD_BF:
    case(BF, "od " + o + " bf")
EF = binf("ef", bytes(range(0, 48)) + bytes([0x7e, 0x7f, 0x80, 0xff, 0xfe, 0x81, 0x80, 0x00, 0x01, 0x82]))
for o in ["-c", "-a", "-t x1", "-t d1", "-t o1", "-t u1", "-t x1z", "-t cz", "-t az", "-t d2", "-t d4", "-t d8", "-t u2", "-t u4", "-t u8", "-t o2", "-t x8",
          "-t o4", "-t o8", "-t x4", "-t x2z", "-t d1 -t d2 -t d4", "-t c -t d1", "-j 33 -N 3 -c", "-j 7 -N 7 -c", "-w8 -t d1", "-w32 -a", "-w32 -c"]:
    case(EF, "od " + o + " ef")
OD_ERR = [
    "-t q", "-t a1", "-t c2", "-t x1,c", "-t x3", "-t x16", "-t x1x3", "-t x0", "-t xzz", "-t zx", "-t x1z1", "-A q", "-N x",
    "-j x", "-N 3x", "-N -1", "-N 0x", "-N 08", "-N 99999999999999999999999", "-j 99999999999999999999999", "-wabc", "-w8c", "-vw8c",
    "--width=8c", "--width=", "--width=x", "--width=99999999999999999999999", "-w99999999999999999999999", "-N ''", "-j 100",
    "-j 100 -N 0", "-j 1b", "-j 1k", "-t d1 -t q", "--format=q", "--read-bytes=x", "--read-bytes=3x", "--skip-bytes=3x",
    "--skip-bytes=", "-j 39", "-j 39 -t x1", "-w1k -t x1", "-w2b -t x1", "-w1m", "-N 17179869184G", "-N 16777216T", "-N 16384P", "-N 16E", "-N 1Z", "-N 1Y", "-N 18446744073709551616", "-N 18446744073709552KB", "-j 17179869184G", "-N 4P2", "-N 2xB", "-N 1KiB2", "-N 4294967296", "-j 1Gx", "-t d1 -t x1q",
]
for o in OD_ERR:
    case(BF, "?od " + o + " bf")
case(BF, "?od -t xz -N 7 -w2 -j 39 bf", "?od -w3 -j 39 bf", "?od -w3 -t x4 -N 4 bf")
for o in ["-N 2b", "-j 2b -N 16", "-N 1k", "-N 1K -A d", "-N 1KB -A d", "-N 1KiB -A d", "-N 1m", "-N 1M", "-N 1MB", "-N 1MiB", "-N 1G", "-j 1b -N 8 -c", "-N 3b", "-j 1KB -N 16", "-j 1KiB -N 16"]:
    case("seq 1 3000 > bg", "od " + o + " bg | wc -l", "od " + o + " bg | tail -n 2")
case(BF, "od -j 1000 -c" + " bf" * 40, "od -j 1519 -c" + " bf" * 40, "od -N 20 -j 1000 -t x1" + " bf" * 40)
case(BF, "od -c - - < bf", "od -c - bf - < bf", "cat bf | od -c - -", "od -j 38 -c - - < bf", "?od -j 40 -c - - < bf")
OD_RF = ["", "-v", "-c", "-t x1", "-t x1z", "-t d2 -t x1", "-w8", "-w8 -v", "-A d", "-A x -v", "-An", "-j 16", "-j 17", "-j 70 -c",
         "-N 70", "-N 80", "-N 79 -c", "-N 33 -t x1", "-t x8", "-t c -w4", "-t d4 -w24", "-vt x2 -w4 -N 12", "-j 3 -N 40 -w32 -t x1"]
for o in OD_RF:
    case(RF, "echo -e 'tail\\c' >> rg", "od " + o + " rg")
    case(RF, "od " + o + " rg")
case(RF, "echo -e 'ABC\\c' >> rg", "od rg", "od -c rg", "od -t x1 -j 70 rg")
case("yes ABCDEFGHIJKLMNO | head -n 3 > rh", "echo abcdefghijklmno >> rh", "yes ABCDEFGHIJKLMNO | head -n 3 >> rh", "od rh", "od -c rh", "od -w8 -t x1 rh", "od -v rh | wc -l")
case(ZF, "echo -e 'xyz\\c' >> zf", "od zf", "od -v zf | wc -l", "od -t x1 -N 70 zf", "od -c zf", "od -A d zf", "od -t d8 zf", "od -w4 -t x4 zf | head -n 5")
case(ZF, "od zf", "od -An -w16 -t x1 zf", "od -j 16 -N 32 zf", "od -j 16 -N 64 -v -w16 -t x1 zf | tail -n 2")
# standard input and several files, which are one stream
case(BF, "cat bf | od -c", "cat bf | od -An -t x1", "od -c - < bf", "od -c -j 5 -N 10 - < bf", "cat bf | od -j 5 -t x1", "od -c bf - < bf", "od -tx1 - bf < bf")
case(BF, RF, "cat rg | od", "cat rg | od -v | wc -l", "cat rg | od -j 3 -N 100 -t x1", "od -c bf bf", "od -j 40 -c bf bf", "od -j 38 -c bf bf",
     "od -N 50 -c bf rg", "od -A d bf rg", "od -c rg rg", "od -t x1 -w4 rg bf", "od -j 70 -N 20 -t x1 rg bf rg", "od -j 118 -c bf rg bf")
case(BF, RF, "?od nosuch bf > xo", "cat xo", "?od bf nosuch > xo", "cat xo", "?od nosuch > xo", "cat xo",
     "?od nosuch nosuch2 > xo", "cat xo", "?od -N 0 nosuch bf > xo", "cat xo", "?od -j 100 nosuch > xo", "cat xo",
     "?od -j 100 nosuch bf > xo", "cat xo", "?od -j 1k -N 1 nosuch bf > xo", "cat xo", "?od -c nosuch rg nosuch2 > xo", "cat xo",
     "?od -A n nosuch > xo", "cat xo", "?od -N 0 -A x bf nosuch > xo", "cat xo")
# longer inputs
case("seq 1 3000 > bg", "od bg | wc -l", "od -c bg | tail -n 2", "od -An -t x1 bg | tail -n 1", "od -j 7000 -N 40 -c bg",
     "od -j 0x2000 -N 16 -t x1 bg", "od -j 1k -N 32 -c bg", "od -N 1k bg | tail -n 2", "od -v bg | wc -l", "od -A d -j 13000 bg",
     "od -t d8 bg | tail -n 2", "od -t x1z bg | tail -n 3", "?od -j 20000 bg", "?od -j 14k bg", "od -j 13892 bg", "od -j 13891 bg",
     "od -j 14000 -N 16 -c bg bg", "cat bg bg | od -j 13880 -N 40 -c", "cat bg | od -j 10000 -N 16 -c", "od -j 5k -N 5 -A x -c bg")
case("yes abcdefghijklmno | head -n 2000 > by", "od by", "od -v by | wc -l", "od -c by | tail -n 3", "od -w32 by", "od -w8 -t x4 by | tail -n 2",
     "od -j 3 by | head -n 3", "od -j 3 -t c by | tail -n 3", "od -t x1 -w48 by")
case("seq 1 3000 | od -An -c | tail -n 2", "seq 1 3000 | od -An -t d4 | wc -l", "seq 1 3000 | od -j 12000 -c | tail -n 3")

SF = binf("sf", b"abc\x00hello world\x01\x02test\ttab\nline two\x7fxyz\x80\xffabcde\x1b[31mred\x00end\x00ab\x00abcd")
SF2 = binf("sg", b"\x00\x00second file\x00\x00xyzzy\x00fg\x80hijklm\xffn\x00tail")
SV = "| tr '\\011\\010\\015 ' 'TBR.' | tr '\\200-\\377' '+'"
SO = ["", "-n 2", "-n 3", "-n 5", "-n 10", "-n 1", "-3", "-2", "-8", "-n3", "-n 0x3", "-n 010", "-n +4", "-n ' 5'", "--bytes=3", "--bytes 3",
      "--bytes=0x6", "-n 3 -n 6", "-a", "-a -n 3", "-", "- -n 3", "-f", "-f -n 3", "-t o", "-t d", "-t x", "-o", "-o -t d", "--radix=x",
      "--radix d", "-t x -n 3", "-f -t d", "-ft x", "-e s", "-e S", "-eS", "--encoding=S", "-e S -e s", "-e s -e S", "--all",
      "--print-file-name", "-fa", "-n 3 -t o -f", "-e S -n 2 -t d", "-e S -f", "-n 2147483648", "-t d -t x", "-o -t x",
      "-10", "-n 1 -e S -t x", "-- ", "-n 3 -- ", "-an3", "-fn3", "-3 -f", "-t d -- ", "--bytes=3 -f", "-n 3 -a - "]
for o in SO:
    case(SF, "strings " + o + " sf " + SV)
for o in ["", "-f", "-t d", "-f -t x -n 3", "-e S -n 2", "-n 2"]:
    case(SF, SF2, "strings " + o + " sf sg " + SV, "strings " + o + " sg sf sg " + SV)
    case(SF, "cat sf | strings " + o + " " + SV, "strings " + o + " < sf " + SV, "strings " + o + " - sf < sf " + SV)
SERR = ["-n 0", "-n x", "-n 4k", "-n 4294967296", "-n 4294967295", "-n -3", "-n ''", "-n ' '", "-n 0x", "-n 08", "-n 99999999999999999999",
        "-n 99999999999999999999x", "--bytes=0", "--bytes=x", "-0", "-n -4294967296", "-n 1.5", "-n 4abc"]
for o in SERR:
    case(SF, "?strings " + o + " sf")
case(SF, "?strings nosuch sf > so", "cat so " + SV, "?strings -f nosuch sf > so", "cat so " + SV, "?strings sf nosuch > so", "cat so " + SV,
     "?strings nosuch > so", "cat so", "mkdir sd", "?strings sd sf > so", "cat so " + SV, "?strings -f sf sd > so", "cat so " + SV,
     "?strings -t d nosuch sf nosuch2 > so", "cat so " + SV)
case("seq 1 3000 > bg", "strings bg | wc -l", "strings -n 20 bg | wc -l", "strings -n 1 bg | wc -l", "strings -t d bg | tail -n 2",
     "strings -f -t x bg | tail -n 2", "cat bg | strings -n 3 | tail -n 2")
case("seq 1 3000 | paste -s -d x > bl", "strings bl | wc -c", "strings -n 13892 bl | wc -c", "strings -n 13893 bl | wc -c",
     "strings -n 3000 bl | wc -c", "strings -t d bl | wc -c", "strings -n 5000 -t d bl | wc -c", "cat bl bl | strings | wc -c",
     "cat bl bl | strings -t x | cut -c 1-12", "strings -n 2147483647 bl | wc -c", "strings -n 4294967294 bl | wc -c")
case("yes | head -n 3000 | tr 'y\\n' 'a\\000' > ba", "strings ba | wc -l", "strings -n 1 ba | wc -l", "strings -n 1 -t d ba | tail -n 2",
     "yes | head -n 600 | tr 'y\\n' 'a\\011' > bt", "strings -n 1 bt | wc -c", "strings -n 5 -t d bt | wc -c")

# -------------------------------------------------------------- xargs
# Inputs are raw: \\ is a backslash for xargs to see, \n a newline.
XI = [r"a b c", r"a\nb\nc", r"  a   b  \n\n c ", r"a\tb", r"'a b' c", r'"a b" c', r"a\\ b c", r"a\\\nb c", r"''", r'"" a',
      r'ab"c d"e f', r"""'a"b' "c'd" """, r"-n -x", r"a\\", r"a\\\\b", r"'a\\b'", r'"a\\b"', r"'' '' x", r"x '' y", r"a\\'b c",
      r'a\\"b', r"\\a\\b", r"one\n\ntwo\n  \nthree", r"a\r\nb", r"\xc3\xa9 \xc3\xbc"]
def od(i):  # bytes the terminal would change (or drop) come out through od
    return " | od -c" if "\\xc3" in i or "\\r" in i else ""
for i in XI:
    for x in ["xargs echo", "xargs -n1 echo '[x]'", "xargs -n 2 echo '[x]'", "xargs -I{} echo '<{}>'", "xargs -t echo", "xargs"]:
        case("echo -e '" + i.replace("'", r"\x27") + "' | " + x + od(i))
for i in [r"a 'b", r'a "b', r"'ab", r'"ab', r"a 'b\nc'", r'a "b\nc" d', r"a b\n'c", r"a b\nc 'd e\nf'", r"x\\'y 'z", r"'a' b 'c", r"a\n'b"]:
    for x in ["xargs echo", "xargs -n1 echo '[x]'", "xargs -n 2 echo '[x]'", "xargs -I{} echo '<{}>'", "xargs -t echo"]:
        # GNU forks and reads on, so a bad quote on a later line is reported before the output of an -I run
        if "-I" in x and "\\n" in i and "'" not in i.split("\\n")[0] and '"' not in i.split("\\n")[0]:
            continue
        case("?echo -e '" + i.replace("'", r"\x27") + "' | " + x)
# no trailing newline, and empty input
for i in ["a b", "a b c d e", "a", "a\\\\", "'a b'"]:
    for x in ["xargs echo", "xargs -n2 echo", "xargs -I{} echo '<{}>'"]:
        case("echo -n -e '" + i.replace("'", r"\x27") + "' | " + x)
# empty items at the very end are dropped, and so is a quote opened there
for i in [r"''", r'""', r"a ''", r'a ""', r"a '' b", r"a '' ", r"'' ", r"'' a", r'"', r"'", r"a '", r' "', r"a\n'", r"a\n''", r'a\n"', r"''\n", r"a ''\n", r"\n\n'"]:
    for x in ["xargs -n1 echo '[x]'", "xargs echo", "xargs -I{} echo '<{}>'"]:
        case("?echo -n -e '" + i.replace("'", r"\x27") + "' | " + x)
# an unclosed quote at the very end of the input, with no newline after it
for i in [r"a 'b", r'a "b', r"'ab", r'"', r"a b 'c d"]:
    for x in ["xargs echo", "xargs -n1 echo '[x]'", "xargs -I{} echo '<{}>'", "xargs -t echo"]:
        case("?echo -n -e '" + i.replace("'", r"\x27") + "' | " + x)
for x in ["xargs echo hi", "xargs -r echo hi", "xargs -I{} echo hi", "xargs -n2 echo hi", "xargs -0 echo hi", "xargs -0 -r echo hi", "xargs -t echo hi",
          "xargs --no-run-if-empty echo hi", "xargs -rt echo hi", "xargs -r", "xargs", "xargs -0"]:
    case("echo -n | " + x, "echo | " + x, "echo -e '  \\n\\t\\n' | " + x)
# the item count
for n in ["1", "2", "3", "5", "7", "40", "100", "01"]:
    case("seq 1 20 | xargs -n " + n + " echo", "seq 1 20 | xargs -n" + n + " echo '[x]'", "seq 1 20 | xargs --max-args=" + n + " echo")
case("seq 1 20 | xargs --max-args 6 echo", "seq 1 20 | xargs -n 4 -n 3 echo", "seq 1 20 | xargs -n3 -t echo > so")
case("seq 1 40 | xargs echo", "seq 1 40 | xargs echo a b c", "seq 1 40 | xargs -n 9 echo a b c", "seq 1 40 | xargs -n 1 echo | wc -l", "seq 1 40 | xargs | wc -c")
case("echo a | xargs -n 2 -I{} -I{} echo '<{}>'", "echo a | xargs -I{} -I{} echo '<{}>'", "echo a | xargs -n 2 -n 3 -I{} -I{} echo '<{}>'", "echo a | xargs -I{} -n 2 -n 3 echo '<{}>'")
case("seq 1 5 | xargs -n 4294967297 echo", "seq 1 5 | xargs -n 4294967296 echo", "seq 1 5 | xargs -n 99999999999 echo")
case("seq 1 14 | xargs -n 2 -I{} echo [{}]", "seq 1 14 | xargs -I{} -n 2 echo [{}]", "seq 1 6 | xargs -n 2 -I{} echo [{}]", "seq 1 6 | xargs -I{} -n 2 echo [{}] {}",
     "seq 1 6 | xargs -n2 --replace echo [{}]", "seq 1 6 | xargs -n2 -i echo [{}]")
# -I
for t in ["-I{}", "-I {}", "-I @", "-I XX", "-I'{}'", "-i", "-i@", "--replace", "--replace=%", "-I{} -r", "-rI{}", "-tI{}", "-I {} -t"]:
    case("echo -e 'a b\\n  c  d \\n\\n\\t e\\nf' | xargs " + t + " echo '<{}>' '<@>' '<XX>' '<%>' x{}y{}z")
case("echo -e 'a\\nb' | xargs -I{} echo {}{} {} x{}y", "?echo -e 'a\\nb' | xargs -I{} {} hi")
case("echo -e 'a\\nb' | xargs -I{} echo x", "echo -e 'a\\nb' | xargs -I{} echo", "echo -e 'a\\nb' | xargs -I{} echo -n {}")
case("echo -e '{}\\n{{}}' | xargs -I{} echo {} x{}", "echo -e 'X\\nXX' | xargs -I X echo X-X", "echo -e 'aaa' | xargs -I aa echo [aa]", "echo -e 'b' | xargs -I ab echo aab abab")
case("echo -e '\"q q\" \\x27r r\\x27\\ns\\\\ t\\n  \"\"\\n\\\\\\n' | xargs -I{} echo '<{}>'")
case("echo -e 'a b\\x00 c d \\x00e' | xargs -0 -I{} echo '<{}>'", "echo -e 'a b\\x00 c d \\x00e\\x00' | xargs -0 -I{} echo '<{}>'",
     "echo -e '\\x00a\\x00\\x00b' | xargs -0 -I{} echo '<{}>'", "echo -e 'a\\nb\\x00c' | xargs -0 -I{} echo '<{}>'", "echo -e 'a\\x00' | xargs -0 -I{} echo '<{}>'")
# -0
for i in [r"a b\x00c\nd\x00", r"a\x00\x00b", r"a\x00", r"\x00", r"\x00\x00", r"a", r"a b\x00", r"'a b'\x00\"c\"\x00\\\\d", r"\x00a", r" a \x00 b ", r"a\nb\nc\n", r"\xc3\xa9\x00\xc3\xbc\x00"]:
    for x in ["xargs -0 echo '[x]'", "xargs -0 -n1 echo '[x]'", "xargs -0 -n2 echo '[x]'", "xargs --null -n1 echo", "xargs -0t -n1 echo"]:
        case("echo -n -e '" + i + "' | " + x + od(i))
# -t
Q = [r"a b", r"c$d", r"e;f", r"*", r"~x", r"x~", r"#x", r"x#", r"a=b", r"=a", r"x\x27y", r"x\x22y", r"x\x60y", r"x\\\\y", r"a!b", r"a|b", r"a&b", r"a(b", r"a)b",
     r"a<b", r"a>b", r"a?b", r"a[b", r"a]b", r"a^b", r"a{b", r"a}b", r"a%b", r"a@b", r"a:b", r"a,b", r"a+b", r"-a", r"a-b", r"a.b", r"a/b", r"a_b", r"A9",
     r"a\tb", r"a\x01b", r"\x01", r"\xc3\xa9", r"a\xc3\xa9b", r"x\x27y\x22z", r"a  b", r"\x27", r"\x22", r"\x27\x27", r"a\x27b c", r"a\nb", r"a\x7fb", r"\x07", r"\x08", r"\x0b", r"\x0c", r"a\x27$b", r"a\x27\x60b", r"a\x27\x22b",
     r"a\x27\\\\b", r"\x1b[0m", r"x\x27\x27y", r"\x80", r"\xff",
     r"a\x27\tb", r"a\x27\x01b", r"a\x1fb", r"a\x1eb", r"\x27\tb", r"a\tb\x27", r"a\x27\x27\tb", r"\tb\x27c"]
for q in Q:
    case("echo -n -e '" + q + "\\x00' | xargs -0 -t echo > so")
case("echo -n -e 'a b\\x00\\x00c' | xargs -0 -n1 -t echo > so", "echo -n -e 'a b\\x00c' | xargs -0 -t echo 'x y' '' z > so",
     "echo a | xargs -t echo '$x' 'a b' > so", "echo 'a b' | xargs -t -I% echo 'x y' % > so")
# the command
case("echo 'a b' | xargs -- echo x", "echo 'a b' | xargs -n1 -- echo x", "echo 'a b' | xargs -t -- echo x", "echo '-n -e' | xargs echo", "echo '-x' | xargs echo -n")
case("echo 'a b' | xargs -0 -n1 echo -n", "echo 'a b' | xargs echo -- x", "echo 'a b' | xargs echo --null")
case("echo 'a b' | xargs -rtn1 echo", "echo 'a b' | xargs -n 1 -r -t echo", "echo a | xargs -rtn1 echo")
case("echo a | xargs --verbose echo", "echo 'a b' | xargs --verbose --max-args=1 echo", "echo 'a' | xargs --no-run-if-empty --null echo | od -c")
# exit statuses
case("?echo 'a b c' | xargs -n1 false", "?echo 'a b c' | xargs false", "?echo -n | xargs false", "echo -n | xargs -r false", "?echo | xargs -I{} false", "echo a | xargs true",
     "?echo a | xargs -n1 cat nosuch > so", "cat so")
case("?echo 'a b' | xargs -n1 cat > so", "cat so", "echo hello > xf", "?echo 'xf nosuch xf' | xargs -n1 cat > so", "cat so", "?echo 'xf nosuch xf' | xargs cat > so", "cat so")
case("?echo a | xargs nosuchcmd > so", "cat so", "?echo a | xargs -n1 nosuchcmd x > so", "cat so", "?echo -n | xargs nosuchcmd > so", "cat so", "?echo -n | xargs -r nosuchcmd > so", "cat so")
case("?echo a | xargs -I{} nosuchcmd {} > so", "cat so")
# children read an empty standard input
case("echo -n | xargs cat", "echo -n | xargs wc -c", "echo -n | xargs -0 wc -l", "echo a | xargs -I{} wc -c", "echo a | xargs -I{} cat", "echo -n | xargs head -n 1")
# find | xargs
case("mkdir fd", "echo one > fd/f1", "echo two > fd/f2", "find fd -type f -name f1 | xargs cat", "find fd -type f -name f1 -print0 | xargs -0 cat",
     "find fd -type f | sort | xargs cat", "find fd -type f | sort | xargs -n1 cat", "find fd -type f -print0 | xargs -0 -n1 echo | sort",
     "find fd -type f | sort | xargs -I{} echo got {}", "find fd -type f | sort | xargs -t -n1 cat > so", "cat so", "find fd -name nomatch | xargs -r echo none",
     "find fd -type f | xargs grep -c o | sort", "find fd -name f1 | xargs wc -c")
case("echo hello > sp1", "echo 'a b' > 'sp 2'", "find . -name 'sp*' -print0 | xargs -0 -n1 echo | sort", "find . -name 'sp*' | sort | xargs -n1 echo | wc -l",
     "find . -name 'sp 2' -print0 | xargs -0 cat", "?find . -name 'sp 2' | xargs cat")
# standard input is not shared with the children: more input than one read takes, and a child that would read it
case("seq 1 300 | xargs -I{} wc -c | sort | uniq -c")
# the same wrapped in pipes
case("seq 1 20 | xargs -n5 echo | tail -n 2", "seq 1 20 | xargs -n5 echo | wc -l", "seq 1 20 | xargs -n5 | wc -l", "seq 1 20 | xargs -n5 echo | wc -w",
     "seq 1 6 | xargs -n3 echo | xargs -n1 echo")

# -------------------------------------------------------------- du
# GNU is run with --apparent-size. RingTail counts whole clusters (512 bytes
# on the test disk), which comes out the same in 1K blocks for any file, and
# in -h for files whose size is a multiple of 512 (64 lines of 8 bytes). A
# directory's own space is not counted by RingTail, so the cases that
# list directories keep to the lines about files.
# ---------------------------------------------------------------- base64
# (the parts that decide what is written, byte for byte, are checked against
# GNU base64 in tests/test_b64.c; these are the program: options, files,
# pipes, messages)
for n in range(0, 8):
    case(("echo -e '%s\\c' | base64" % "abcdefg"[:n]) if n else "echo -n | base64")
case("echo hello | base64", "echo hello | base64 -", "echo hello | base64 | base64 -d", "echo hello | base64 | base64 --decode", "echo hello | base64 | base64 --dec",
     "echo hello | base64 | base64 -d -")
case("echo -e '\\xff\\xfe\\x00\\x01' | base64", "echo -e '\\x00' | base64", "echo -e '\\x00\\x00\\x00\\x00\\x00' | base64", "echo -e '\\xff\\xff\\xff' | base64",
     "echo -e '\\xfb\\xef\\xbe' | base64", "echo -e '\\xfb\\xef\\xbe\\xff' | base64")
ALLB = ["\\x%02x" % i for i in range(256)]
# every byte value, in files (the shell takes lines of up to 255 characters)
mkall = ["echo -e '%s\\c' > ba%d" % ("".join(ALLB[k:k + 40]), k // 40) for k in range(0, 256, 40)]
CATALL = "cat ba0 ba1 ba2 ba3 ba4 ba5 ba6"
case(*mkall, CATALL + " | base64", CATALL + " | base64 | base64 -d | od -An -tx1", CATALL + " | base64 -w 0 | base64 -d | od -An -tx1",
     CATALL + " | base64 -w 7 | base64 -d | od -An -tx1", CATALL + " | base64 -w 7 | base64 -di | od -An -tx1", "base64 ba0 | base64 -d | cmp - ba0")
# line length
LONG = "yes abcdefg | head -n 12 | "
case(LONG + "base64", LONG + "base64 -w 0", LONG + "base64 -w 20", LONG + "base64 -w20", LONG + "base64 --wrap=20", LONG + "base64 --wrap 20",
     LONG + "base64 --wra=20", LONG + "base64 --w 20", LONG + "base64 -w 76", LONG + "base64 -w 77", LONG + "base64 -w 1000")
case("echo hello | base64 -w 1", "echo hello | base64 -w 2", "echo hello | base64 -w 3", "echo hello | base64 -w 4", "echo hello | base64 -w 8", "echo hello | base64 -w 9",
     "echo hello | base64 -w 11", "echo hello | base64 -w 12", "echo hello | base64 -w 0", "echo hello | base64 -w -0", "echo hello | base64 -w +3",
     "echo hello | base64 -w ' 3'", "echo hello | base64 -w 007", "echo hello | base64 -w 4294967295", "echo hello | base64 -w 4294967296",
     "echo hello | base64 -w 9223372036854775807", "echo hello | base64 -w 9223372036854775808", "echo hello | base64 -w 99999999999999999999")
case("echo hello | base64 -iw4", "echo hello | base64 -w4 -i", "echo hello | base64 -w 4 -w 8", "echo hello | base64 -w 4 -w 0", "echo hello | base64 -w 0 -w 4",
     "echo -n | base64 -w 4", "echo -n | base64 -w 0", "echo -e 'abc\\c' | base64 -w 4", "echo -e 'abcd\\c' | base64 -w 4", "echo -e 'abc\\c' | base64 -w 2",
     "echo -e 'abcdef\\c' | base64 -w 8", "echo -e 'abcdef\\c' | base64 -w 4")
# decoding
case("echo aGVsbG8K | base64 -d", "echo aGVsbG8K | base64 -di", "echo aGVsbG8K | base64 -id", "echo aGVsbG8K | base64 -d -i", "echo aGVsbG8K | base64 --decode --ignore-garbage",
     "echo aGVsbG8K | base64 --d --i", "echo YQ== | base64 -d", "echo YWI= | base64 -d", "echo YQ==YQ== | base64 -d", "echo -e 'YQ==\\nYQ==' | base64 -d",
     "echo -e 'YW\\nJj\\nZG\\nVm' | base64 -d", "echo -e 'YWJj\\n\\n\\nZGVm\\n\\n' | base64 -d", "echo | base64 -d", "echo -n | base64 -d", "echo -e '\\n\\n' | base64 -d")
case("?echo aGVsbG8 | base64 -d", "?echo aGVsbG8= | base64 -d", "?echo aGVsbG8== | base64 -d", "?echo -n aGVsbG8 | base64 -d", "?echo -n YQ | base64 -d", "?echo -n YQ= | base64 -d",
     "?echo -n Y | base64 -d", "?echo -n YQ=A | base64 -d", "?echo -n YWJ= | base64 -d", "?echo -n 'ab=' | base64 -d", "?echo -n '!!!!' | base64 -d", "?echo -n 'YW!j' | base64 -d",
     "?echo -n 'YWJj!!!!ZGVm' | base64 -d", "?echo -n 'ZGVmYQ==YQ=' | base64 -d", "?echo 'aGVs bG8K' | base64 -d", "?echo -e 'aGVs\\tbG8K' | base64 -d")
case("echo 'aG!Vs@bG8K' | base64 -di", "echo 'aGVs bG8K' | base64 -di", "echo -e 'a G V s\\tb\\xffG 8 K' | base64 -di", "echo 'a=GVsbG8K' | base64 -di | od -c",
     "?echo '!!!' | base64 -di", "?echo 'aGVsbG8' | base64 -di", "echo 'YQ==!!YQ==' | base64 -di", "?echo 'YQ==!!YQ=' | base64 -di", "?echo '=a' | base64 -di")
# files
case("echo hello > bf1", "base64 bf1", "base64 -w 4 bf1", "base64 bf1 -w 4", "base64 -- bf1", "base64 - < bf1", "base64 -w 4 - < bf1", "base64 bf1 > bf2", "base64 -d bf2",
     "base64 -d < bf2", "base64 -d bf2 > bf3", "cmp bf1 bf3", "base64 -d -- bf2", "base64 --decode bf2", "base64 -di bf2")
case("?base64 nosuchfile", "?base64 -d nosuchfile", "?base64 -w 4 nosuchfile")
case("yes abcdefg | head -n 2000 > bf1", "base64 bf1 | wc -l", "base64 -w 0 bf1 | wc -c", "base64 bf1 | base64 -d | cmp - bf1", "base64 -w 5 bf1 | base64 -d | cmp - bf1",
     "base64 bf1 | base64 -di | cmp - bf1", "base64 bf1 | head -n 3", "base64 bf1 | tail -n 2", "base64 bf1 | base64 -d | wc -c")
# through the other tools
case("echo hello | base64 | tr a-z A-Z", "echo hello | base64 | rev", "echo hello world | base64 -w 6 | sort", "echo hello world | base64 | cut -c 1-4",
     "echo hello world | base64 -w 0 | fold -w 6", "echo hello world | base64 -w 6 | tr -d '\\n' | base64 -d")

# ---------------------------------------------------------------- sha256
# (the parts that read a list line, byte for byte, are checked against GNU in
# tests/test_shasum.c; these are the program: hashing, options, lists,
# messages and statuses. The program is GNU's sha256sum, run as sha256.)
import hashlib
def sha(b): return hashlib.sha256(b).hexdigest()
HX1, HX2 = sha(b"hello\n"), sha(b"world\n")
def lst(name, *lines):
    """Commands that write a file of these lines (an empty line is ' ' trimmed by echo, so it is written by a bare echo)."""
    cmds = []
    for i, l in enumerate(lines):
        r = ">" if i == 0 else ">>"
        cmds.append("echo %s %s" % (r, name) if l == "" else "echo -e '%s' %s %s" % (l, r, name))
        assert len(cmds[-1]) < 250, cmds[-1]
    return cmds
SETUP = ["echo hello > x1", "echo world > x2"]
# hashing: what is known, and the lengths around the blocks of 64 bytes (and the 4096 it reads by)
case("echo hello | sha256", "echo -n | sha256", "echo -n abc | sha256", "echo -n 'The quick brown fox jumps over the lazy dog' | sha256", "echo | sha256", "echo -n abc | sha256 -",
     "echo -n abc | sha256 - -")
case(*["yes abcdefg | head -c %d | sha256" % n for n in (1, 2, 54, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000, 4095, 4096, 4097, 8191, 8192, 8193, 10000)])
case("yes abcdefg | head -c 100000 | sha256", "yes | head -c 65536 | sha256")
case("yes abcdefg | head -c 4097 > xf", "sha256 xf", "sha256 < xf", "sha256 - < xf", "cat xf xf | sha256", "sha256 xf xf", "sha256 xf - < xf", "sha256 xf xf xf | wc -l")
ALLB = ["\\x%02x" % i for i in range(256)]
case(*["echo -e '%s\\c' > bs%d" % ("".join(ALLB[i * 40:(i + 1) * 40]), i) for i in range(7)],
     *["sha256 bs%d" % i for i in range(7)], "cat bs0 bs1 bs2 bs3 bs4 bs5 bs6 | sha256", "sha256 bs0 bs1 bs2 bs3 bs4 bs5 bs6")
# options for what is written
case(*SETUP, "sha256 x1", "sha256 x1 x2", "sha256 -b x1", "sha256 -t x1", "sha256 -bt x1", "sha256 -tb x1", "sha256 -b -t x1", "sha256 -t -b x1", "sha256 --binary x1",
     "sha256 --text x1", "sha256 --bin x1", "sha256 --binary --text x1", "sha256 --text --binary x1", "sha256 -b x1 x2", "echo hello | sha256 -b", "echo hello | sha256 -b -")
case(*SETUP, "sha256 --tag x1", "sha256 --tag x1 x2", "sha256 --ta x1", "sha256 --tag -b x1", "sha256 -b --tag x1", "sha256 -t --tag x1", "sha256 --tag -t -b x1", "sha256 --tag -- x1",
     "echo hello | sha256 --tag", "echo hello | sha256 --tag -", "sha256 --tag --tag x1")
case(*SETUP, "sha256 -z x1 | od -c", "sha256 --zero x1 x2 | od -c", "sha256 --ze x1 | od -c", "sha256 -z --tag x1 | od -c", "sha256 -zb x1 | od -c", "sha256 -bz x1 | od -c",
     "echo hello | sha256 -z | od -c", "sha256 -z x1 | tr '\\0' '\\n'")
case(*SETUP, "sha256 -- x1", "sha256 x1 -- x2", "sha256 x1 --", "sha256 -b -- x1")
# files that are not there, or not files
case(*SETUP, "?sha256 nosuch", "?sha256 nosuch x1", "?sha256 x1 nosuch x2", "?sha256 -b nosuch", "?sha256 --tag nosuch x1", "?sha256 nosuch nosuch2", "?sha256 -z nosuch x1 | od -c",
     "mkdir sd", "?sha256 sd", "?sha256 sd x1", "?sha256 x1 sd x2", "?sha256 nosuch/x1")
# a list of what was written
case(*SETUP, "sha256 x1 x2 > l1", "cat l1", "sha256 -c l1", "sha256 -c --quiet l1", "sha256 -c --status l1", "sha256 -c -w l1", "sha256 -c --strict l1", "sha256 -c --warn l1", "sha256 --check l1",
     "sha256 --ch l1", "sha256 -cw l1", "sha256 -c < l1", "sha256 -c - < l1", "cat l1 | sha256 -c", "cat l1 | sha256 -c -", "sha256 -c l1 l1", "sha256 -c --quiet --quiet l1", "sha256 -c --status l1 | wc -c")
case(*SETUP, "sha256 --tag x1 x2 > l1", "cat l1", "sha256 -c l1", "sha256 -c --quiet l1", "sha256 -c -w l1", "sha256 -b x1 x2 > l2", "sha256 -c l2", "sha256 -t x1 x2 > l3", "sha256 -c l3",
     "cat l1 l2 l3 > l4", "sha256 -c l4", "sha256 -c l1 l2 l3", "sha256 -c --status l4")
# a file that changed, or went
case(*SETUP, "sha256 x1 x2 > l1", "echo changed > x1", "?sha256 -c l1", "?sha256 -c --quiet l1", "?sha256 -c --status l1", "?sha256 -c -w l1", "?sha256 -c --strict l1", "sha256 x1 > l2", "sha256 -c l2",
     "?sha256 -c --ignore-missing l1", "rm x2", "?sha256 -c l1", "?sha256 -c --quiet l1", "?sha256 -c --status l1", "?sha256 -c --ignore-missing l1", "?sha256 -c --ignore-missing --quiet l1",
     "?sha256 -c --ignore-missing --status l1", "?sha256 -c --ignore-m l1", "sha256 -c l2 | wc -l")
case(*SETUP, "sha256 x2 > l2", "rm x2", "?sha256 -c l2", "?sha256 -c --ignore-missing l2", "?sha256 -c --ignore-missing --status l2", "?sha256 -c --ignore-missing --quiet l2",
     "?sha256 -c --ignore-missing --strict l2", "echo world > x2", "sha256 -c --ignore-missing l2")
case(*SETUP, "mkdir sd", *lst("l1", HX1 + "  x1", HX1 + "  sd", HX2 + "  x2"), "?sha256 -c l1", "?sha256 -c --status l1", "?sha256 -c --quiet l1", "?sha256 -c --ignore-missing l1",
     *lst("l2", HX1 + "  sd"), "?sha256 -c --ignore-missing l2", "?sha256 -c --ignore-missing --status l2")
# lists written by hand: the ways of writing a line, and the lines that are not
case(*SETUP, *lst("l1", HX1 + "  x1", HX1 + " x1", HX1 + "\\tx1", HX1 + " *x1", HX1.upper() + "  x1", HX1 + "  x1\\x0d"), "sha256 -c l1", "sha256 -c --quiet l1", "sha256 -c -w l1")
case(*SETUP, *lst("l1", "# a comment", "", HX1 + "  x1", "#" + HX1 + "  x2", HX2 + "  x2"), "sha256 -c l1", "sha256 -c -w l1", "sha256 -c --strict l1", "sha256 -c --status l1")
case(*SETUP, *lst("l1", " " + HX1 + "  x1", "\\t" + HX1 + "  x1"), "sha256 -c l1", *lst("l2", "SHA256 (x1) = " + HX1, "SHA256 (x2) = " + HX2, "SHA256(x1)= " + HX1), "sha256 -c l2", "sha256 -c -w l2",
     *lst("l3", "  SHA256 (x1) = " + HX1, "SHA256 (x1) =  " + HX1, "SHA256 (x1)\\t=\\t" + HX1, "SHA256 (x1) = " + HX1.upper()), "sha256 -c l3", "sha256 -c -w l3")
case(*SETUP, *lst("l1", "junk", HX1 + "  x1", HX1[:63] + "  x1"), "sha256 -c l1", "sha256 -c -w l1", "?sha256 -c --strict l1", "sha256 -c --status l1", "?sha256 -c --status --strict l1", "sha256 -c --quiet -w l1",
     "?sha256 -c --strict l1", "?sha256 -c --strict -w l1", "?sha256 -c --strict --status l1")
case(*SETUP, *lst("l1", "junk"), "?sha256 -c l1", "?sha256 -c -w l1", "?sha256 -c --status l1", "?sha256 -c --strict l1", "?sha256 -c --ignore-missing l1",
     *lst("l2", HX1 + "x1", HX1, HX1[:62] + "zz  x1", "sha256 (x1) = " + HX1, "SHA1 (x1) = " + HX1, "SHA256 (x1) = " + HX1[:63]), "?sha256 -c l2", "?sha256 -c -w l2")
case(*SETUP, "echo -n > le", "?sha256 -c le", "?sha256 -c -w le", "?sha256 -c --status le", "echo '# nothing' > l1", "?sha256 -c l1", "echo > l2", "?sha256 -c l2",
     "?sha256 -c < le", "?sha256 -c - < le", "?sha256 -c -w - < le", "?echo junk | sha256 -c", "?echo junk | sha256 -c -w", "?echo junk | sha256 -c -w -")
case(*SETUP, "echo -n -e '%s  x1' > l1" % HX1, "sha256 -c l1", "echo -n -e '%s  x2\\n%s  x1' > l2" % (HX2, HX1), "sha256 -c l2", "echo -n -e '%s  x1\\x0d' > l3" % HX1, "sha256 -c l3",
     "echo -n -e '%s\\x0d' > l4" % HX1, "?sha256 -c l4", "?sha256 -c -w l4", "echo -n -e '%s  x1\\x0d\\n' > l5" % HX1, "sha256 -c l5")
case(*SETUP, *lst("l1", HX1 + "  x1", HX2 + "  x1", HX1[:63] + "0  x1", HX1 + "  x2"), "?sha256 -c l1", "?sha256 -c --quiet l1", "?sha256 -c --status l1", "?sha256 -c --quiet --status l1", "?sha256 -c --status --quiet l1",
     "?sha256 -c -w --quiet l1", "?sha256 -c --quiet -w l1", "?sha256 -c -w --status l1", "?sha256 -c --status -w l1")
case(*SETUP, *lst("l1", "junk", HX1 + "  x1", HX2 + "  x1", HX1 + "  nosuch", "more junk"), "?sha256 -c l1", "?sha256 -c --quiet l1", "?sha256 -c --status l1", "?sha256 -c --strict l1",
     "?sha256 -c -w l1", "?sha256 -c --ignore-missing l1", "?sha256 -c --quiet -w l1", "?sha256 -c --warn --quiet l1", "?sha256 -c --quiet --warn l1", "?sha256 -c --status --warn l1",
     "?sha256 -c --warn --status l1")
# GNU settles from the first line that shows it whether the lines have a mode character after the blank, and holds the rest to it, across lists too
# (names with a space or a "*" in them would be written in shell quotes by GNU, so the lines that read as such a name are not run)
case(*SETUP, *lst("m1", HX1 + "  x1", HX1 + " x1", HX1 + "\\tx1", HX1 + "  x1"), "?sha256 -c m1", "?sha256 -c -w m1", "?sha256 -c --strict m1", "?sha256 -c --status m1",
     *lst("m2", HX1 + " x1", HX1 + "\\tx1", HX1 + "\\tx1"), "sha256 -c m2", "sha256 -c -w m2",
     *lst("m3", HX1 + " *x1", HX1 + "  x1", HX1 + " x1"), "?sha256 -c m3", "?sha256 -c -w m3",
     "?sha256 -c m1 m2", "?sha256 -c -w m3 m2", "?sha256 -c --quiet m1 m3", "sha256 x1 > m4", "sha256 -c m4 m1 m4", "?sha256 -c m4 m2 m4")
case(*SETUP, *lst("m1", "SHA256 (x1) = " + HX1, HX1 + " x1", HX1 + "\\tx1"), "sha256 -c m1", "sha256 -c -w m1",
     *lst("m2", "SHA256 (x1) = " + HX1, HX1 + "  x1", HX1 + " x1"), "?sha256 -c m2", "?sha256 -c -w m2",
     *lst("m3", "junk", HX1, "# " + HX1 + "  x1", "", HX1 + " x1", HX1 + "\\tx1"), "sha256 -c -w m3",
     *lst("m4", "junk", HX1, "# " + HX1 + "  x1", "", HX1 + "  x1", HX1 + " x1"), "?sha256 -c -w m4")
case(*SETUP, *lst("m1", HX1 + "  x1"), *lst("m2", HX1 + " x1"), "sha256 x1 > m6", "echo hello | sha256 > m3", "sha256 -c m1 - < m6", "sha256 -c - m1 < m6", "?sha256 -c m1 - < m2", "?sha256 -c -w m1 - < m2",
     "?sha256 -c m1 - < m3", "?sha256 -c -w - < m3", "?cat m3 | sha256 -c -w", "echo hello | sha256 -c m3", "sha256 -c m6 m6 m6", "sha256 -c m1 m1", "?sha256 -c m1 m2 m1", "?sha256 -c -w m1 m2 m1",
     "?cat m2 | sha256 -c -w m1 -")

# the lists named: none, several, missing, a directory, standard input
case(*SETUP, "sha256 x1 > l1", "sha256 x2 > l2", "sha256 -c l1 l2", "?sha256 -c l1 nosuch l2", "?sha256 -c nosuch", "?sha256 -c nosuch l1", "mkdir sd", "?sha256 -c sd", "?sha256 -c sd l1", "?sha256 -c l1 sd",
     "sha256 -c l1 - < l2", "sha256 -c - l1 < l2", "?sha256 -c x1", "?sha256 -c x1 l1", "?sha256 -c l1 x1", "?sha256 -c -w x1 l1")
case(*SETUP, "sha256 x1 > l1", "echo hello | sha256 > l2", "cat l2", "echo hello | sha256 -c l2", "?sha256 -c l2 < x1", "sha256 - < x1 > l3", "sha256 -c l3 < x1", "echo hello | sha256 -c l3", "echo other | sha256 -c l3 | wc -l")
# all the options that go with each other (and a long one for each)
case(*SETUP, "sha256 x1 > l1", "sha256 -c --check l1", "sha256 --check --quiet l1", "sha256 --check --stat l1", "sha256 --check --status l1", "sha256 --check --str l1", "sha256 --check --strict l1",
     "sha256 --check --ig l1", "sha256 --check --ignore-missing l1", "sha256 --check --wa l1", "sha256 --check --warn l1", "sha256 --check --qu l1", "sha256 -cw --quiet --strict --ignore-missing l1",
     "sha256 -c -- l1", "sha256 --c -- l1", "sha256 -c l1 -w", "sha256 -c --strict l1 --quiet", "sha256 l1 -c")

def mk(name, lines):
    return "yes abcdefg | head -n %d > %s" % (lines, name) if lines else "echo -n > " + name
SIZES = [0, 1, 63, 64, 65, 127, 128, 129, 200, 1000, 1280, 1281, 5000, 16384]
files = [mk("f%d" % n, n) for n in SIZES]
names = " ".join("f%d" % n for n in SIZES)
case(*files, "du " + names, "du -k " + names, "du -h f64 f128 f1280 f16384", "du -s f128 f1000", "du -c f64 f128 f1280", "du -kc f1280 f16384")
H = [64, 128, 192, 640, 1280, 1344, 2560, 16384, 20000, 65536, 131072]
case(*[mk("g%d" % n, n) for n in H], "du -h " + " ".join("g%d" % n for n in H), "du -hc g64 g128", "du -ch g1280 g16384")
case("mkdir dir", "mkdir dir/sub", mk("dir/f1", 192), mk("dir/sub/f2", 1344), mk("dir/sub/f3", 64), mk("dir/f4", 0),
     "du -a dir | grep /f | sort", "du -ah dir | grep /f | sort", "du -a dir/ | grep /f | sort", "du -a dir/sub | grep /f | sort",
     "du dir/sub/f2 dir/f1", "du -a | grep /f | sort", "du -a . | grep /f | sort", "du -ah ./dir | grep /f | sort")

# ---------------------------------------------------------------- printf
# (the directives, the numbers and the escapes are checked one by one against GNU in
# tests/test_pfcore.c; these are the program: the arguments reaching the format,
# standard output and standard error in order, the statuses. The program is GNU's
# printf, run as printf by name. %q and the floating point conversions are not
# in RingTail's printf: they are in qemu_test.py.)
case("printf 'hello\\n'", "printf hello", "printf '%s\\n' abc", "printf '%s\\n' 'two words' three", "printf '[%s]\\n'", "printf '[%d]\\n'", "printf '[%c]\\n' | od -c",
     "printf '%s %s\\n' a b c d e", "printf '%%\\n'", "printf '100%%\\n' x y", "printf 'a\\tb\\\\c\\n'", "printf '%s\\n' 'a\\tb'", "printf '%b\\n' 'a\\tb\\\\c'")
case("printf '%d\\n' 1 -2 +3 0x1f 010 0b101 0XFF -0x10", "printf '%i|%u\\n' 5 7 -5 -7", "printf '%x %X %o\\n' 255 255 8 3054 3054 64", "printf '%#x %#X %#o\\n' 255 255 8", "printf '%#x|%#o\\n' 0 0",
     "printf '%d\\n' 9223372036854775807 -9223372036854775808", "printf '%u\\n' 18446744073709551615 -1", "printf '%x\\n' -1", "printf '%d\\n' ''", "printf '%d %d\\n' 1")
case("printf '[%5d]\\n' 42", "printf '[%-5d]\\n' 42", "printf '[%05d]\\n' 42 -42", "printf '[%+d]\\n' 42 -42 0", "printf '[% d]\\n' 42 -42", "printf '[%.5d]\\n' 42 -42", "printf '[%8.5d]\\n' 42 -42",
     "printf '[%-8.5d]\\n' 42", "printf '[%08.5d]\\n' 42", "printf '[%.0d]\\n' 0 1", "printf '[%+.0d]\\n' 0", "printf '[%#8x]\\n' 255", "printf '[%#08x]\\n' 255", "printf '[%-#8o]\\n' 8")
case("printf '[%s]\\n' abc", "printf '[%8s]\\n' abc", "printf '[%-8s]\\n' abc", "printf '[%.2s]\\n' abc", "printf '[%8.2s]\\n' abc", "printf '[%-8.2s]\\n' abc", "printf '[%.0s]\\n' abc", "printf '[%.s]\\n' abc",
     "printf '[%c]\\n' abc", "printf '[%5c]\\n' abc", "printf '[%-5c]\\n' abc", "printf '[%c%c%c]\\n' a b c", "printf '[%5s]\\n' ''")
case("printf '[%*d]\\n' 6 42", "printf '[%*d]\\n' -6 42", "printf '[%-*d]\\n' 6 42", "printf '[%.*d]\\n' 5 42", "printf '[%.*d]\\n' -5 42", "printf '[%*.*d]\\n' 8 5 42", "printf '[%*s]\\n' 6 ab", "printf '[%.*s]\\n' 1 abc",
     "printf '[%*s]\\n' 3", "printf '[%*.*s]\\n' 6 2 abcdef")
case("printf 'a\\x41b\\n'", "printf '\\x41\\x4a\\x4A\\n'", "printf '\\101\\60\\0601\\n'", "printf 'a\\0b' | od -c", "printf '\\e[0m' | od -c", "printf '\\a\\b\\f\\r\\v' | od -c", "printf 'one\\ntwo\\n'", "printf '\\\"q\\\"\\n'",
     "printf '\\q\\z\\n'", "printf 'x\\cy\\n'", "printf 'x\\n\\cy'", "printf '%s\\cn\\n' abc def", "printf '\\u00e9\\n' | od -c", "printf '\\u0041\\U00000042\\n'", "printf '\\u20ac\\U0001F600\\n' | od -c")
case("printf '%b\\n' 'a\\nb'", "printf '%b\\n' 'a\\0101b'", "printf '%b\\n' 'a\\101b'", "printf '%b\\n' 'a\\x41b'", "printf '%b|%b\\n' 'x\\cy' z", "printf '%b\\n' '\\\\' 'a\\tb' | od -c", "?printf '%5b|\\n' ab", "printf '%s\\n' 'a\\nb'",
     "printf '%b' 'a\\0b' | od -c", "printf '%b\\n' 'tab\\there' '\\e[1m' | od -c")
case("printf '%d\\n' \"'A\"", "printf '%d\\n' '\"A'", "printf '%d %d\\n' \"'a\" \"'ab\"", "printf '%x\\n' \"'a\"", "?printf '%d\\n' \"'\"", "printf '%c\\n' 65", "printf '%s\\n' \"'a\"")
case("?printf '%d\\n' abc", "?printf '%d %d\\n' 1 2x 3", "?printf '%d\\n' 12abc", "?printf '%d\\n' 99999999999999999999", "?printf '%u\\n' 99999999999999999999", "?printf '%x\\n' -99999999999999999999",
     "?printf '%d\\n' 0x", "?printf '%d\\n' 08", "?printf '%d\\n' ' 5'", "?printf '%d\\n' '5 '", "?printf '%d\\n' 1 abc 3")
case("printf '%d\\n' \"'ab\"", "printf '%s\\n' a", "?printf '%d\\n' 'x'", "printf '%d%%\\n' 5", "printf '%s\\n'", "printf 'x%sy%sz\\n' 1", "printf '%s-%s\\n' a b c", "printf '%s\\n' a b c", "printf '\\n' extra", "printf 'no directive' a b c d",
     "printf '%s,' a b c | od -c", "printf '%d %s\\n' 1 a 2 b 3")
case("?printf '%y\\n' 1", "?printf 'a%'", "?printf '%'", "?printf '%5'", "?printf '%-'", "?printf '%l\\n' 1", "?printf '%hhd\\n' 5", "?printf '%ld|%lld|%jd|%zd|%td\\n' 1 2 3 4 5", "?printf '%+s\\n' a", "?printf '%#s\\n' a", "?printf '%0s\\n' a",
     "?printf '%.3c\\n' a", "?printf '%0c\\n' a", "?printf '%#d\\n' 1", "?printf '%+x\\n' 1", "?printf '%-5%\\n'", "?printf '%5b\\n' a")
case("?printf '\\x\\n'", "?printf '\\xg\\n'", "?printf 'a\\u12\\n'", "?printf '\\ud800\\n'", "?printf '\\uD800\\n'", "?printf '%b\\n' '\\x'", "?printf '%b\\n' '\\u12'", "printf 'ok\\n' x",
     "?printf '%d\\n' x", "?printf '%d\\n' x 7", "?printf '%s %d\\n' a b c", "?printf '%y %d\\n' 1 2")
case("?printf '[%*d]\\n' abc 5", "?printf '[%.*d]\\n' abc 5", "?printf '[%*d]\\n' 99999999999 5", "?printf '[%.*d]\\n' 99999999999 5", "?printf '[%*d]\\n' 2147483648 5", "?printf '[%*d]\\n' -2147483648 5", "?printf '[%*d]\\n' 2147483647 5",
     "?printf '[%2147483647d]\\n' 5", "?printf '[%2147483648d]\\n' 5", "?printf '[%.2147483647d]\\n' 5", "?printf '[%.2147483648d]\\n' 5", "?printf '[%*.0s]\\n' -2147483648 abc",
     "?printf 'a\\n[%2147483647s]\\nz\\n' x", "?printf 'a\\n[%2147483647s]\\nz\\c' x")
case("?printf -- '%s\\n' a b", "?printf -- -", "?printf -- --", "?printf -- '-%s\\n' a", "?printf '-%s\\n' a b", "?printf -n", "?printf -e", "?printf '--%s\\n' a", "?printf -- -- a", "?printf '%s\\n' -- a --")

# -------------------------------------------------------------- cal
# The oracle is BSD's cal (Debian's ncal package), not util-linux's: the
# layout, the Julian calendar up to September 1752 and the argument rules
# are RingTail's, and util-linux prints differently. Only fixed months and
# years go here, since "now" cannot be compared; the no-argument forms and
# the highlight are in qemu_test.py. Errors are piped through cat so that
# the message is compared and the exit status (64 there, 2 here) is not.
_c = subprocess.run(["cal", "9", "1752"], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C")).stdout.splitlines()
if _c[:2] != ["   September 1752     ", "Su Mo Tu We Th Fr Sa  "]:
    sys.exit("tests/gen_text_cases.py needs BSD cal (the ncal package) as cal, not util-linux's")
MON = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]
# every month of a year, by number and by name
case(*["cal %d 2026" % m for m in range(1, 13)])
case(*["cal %s 2026" % n for n in MON])
case(*["cal %d 1752" % m for m in range(1, 13)])
case(*["cal %d 1582" % m for m in (9, 10, 11)], "cal 2 1700", "cal 2 1900", "cal 2 2000", "cal 2 2100", "cal 2 2024", "cal 2 2023", "cal 2 1600", "cal 2 1500",
     "cal 2 1751", "cal 2 1752", "cal 2 1753", "cal 2 4", "cal 2 100", "cal 2 400", "cal 2 8", "cal 2 9996")
# the edges of the range
case("cal 1 1", "cal 12 1", "cal 3 1", "cal 1 2", "cal 12 9999", "cal 1 9999", "cal 2 9999", "cal 9 9998", "cal 2 9996", "cal 12 100", "cal 1 101", "cal 10 1000")
# a week or two either side of the missing days, and their neighbours
case("cal 8 1752", "cal 9 1752", "cal 10 1752", "cal 9 1751", "cal 9 1753", "cal 9 1752 | grep -c 14", "cal 9 1752 | wc -l",
     "cal 9 1752 | sed -n 3p", "cal 9 1752 | cut -c1-20 | tail -n 3")
# one weekday for each of a spread of dates
case(*["cal 1 %d | sed -n 3p" % y for y in (1, 100, 1000, 1500, 1582, 1700, 1751, 1752, 1753, 1800, 1900, 1970, 2000, 2024, 2038, 2100, 2400, 5000, 9999)])
# whole years
case("cal 2026", "cal 2024", "cal 2023", "cal 2000", "cal 1900", "cal 1752", "cal 1751", "cal 1753")
case("cal 1", "cal 4", "cal 100", "cal 1582", "cal 1700", "cal 9999", "cal 9996", "cal 1000", "cal 12", "cal 12345 | cat")
case("cal 2026 | wc -l", "cal 2026 | wc -c", "cal 3 2026 | wc -l", "cal 3 2026 | wc -c", "cal 2026 | head -n 3", "cal 2026 | tail -n 3", "cal 1752 | sed -n 20,27p",
     "cal 3 2026 | cut -c1-20", "cal 3 2026 | grep -c ' 1 '", "cal 3 2026 | od -c | head -n 6")
# -y, with the month ignored
case("cal -y 2026", "cal -y 3 2026", "cal -y mar 1752", "cal -y 9999", "cal -y 1", "cal -y 2000 | wc -l")
case("cal -y 3 2026 > ya", "cal 2026 > yb", "cmp ya yb", "cal -y 2026 | wc -c", "cal -y 1 | wc -l", "cal -y -y 2026 | wc -l")
# when more than one of -1, -3 and -y is given, the last one counts
case("cal -1 -3 3 2026", "cal -3 -1 3 2026", "cal -y -3 3 2026", "cal -3 -y 3 2026", "cal -1 -y 3 2026 | head -n 2", "cal -y -1 3 2026", "cal -31y 3 2026 | head -n 2",
     "cal -y31 3 2026 | head -n 2", "cal -3 -1 3 2026 | wc -l", "cal -1 -3 -1 3 2026 | wc -l", "cal -1 3 -3 2026 | wc -l", "cal -3 -y 2026 | head -n 1", "cal -y -3 2026 | cat",
     "cal -1 -y 2026 | head -n 1", "cal -y -1 2026 | cat")
# leap years by the Gregorian rule: every 4th, except centuries not divisible by 400 (the 200s are the ones a wrong rule would take for leap years)
case(*["cal 2 %d | tail -n 3" % y for y in (1800, 2200, 2600, 3000, 3400, 1804, 2004, 2096, 2400, 2800, 4000, 9200)])
# a word that is not a month, however much of it matches
case("cal xar 2026 | cat", "cal Xay 2026 | cat", "cal zan 2026 | cat", "cal ar 2026 | cat", "cal ep 2026 | cat", "cal e 2026 | cat", "cal x 2026 | cat", "cal xmar 2026 | cat", "cal mxr 2026 | cat",
     "cal mar 2026 | cat", "cal maR 2026 | cat", "cal mAr 2026 | cat", "cal Mar 2026 | cat", "cal ocT 2026 | cat", "cal Aug 2026 | cat", "cal aUG 2026 | cat", "cal DEC 2026 | cat")
# what counts as a number: spaces before it, a sign, nothing after it
case("cal 3 ' ' | cat", "cal 3 '   ' | cat", "cal ' ' 2026 | cat", "cal 3 '-' | cat", "cal 3 ' -5' | cat", "cal 3 '+ 5' | cat", "cal -- 3 -5 | cat", "cal -- 3 -0 | cat",
     "cal -- 3 +-5 | cat", "cal -- -5 2026 | cat", "cal ' -3' 2026 | cat", "cal 3 '+0' | cat", "cal '  3' 2026", "cal 3 '   2026'", "cal '  +3' ' +2026'", "cal -- -1 2026 | cat",
     "cal -- 3 -2026 | cat", "cal -- -0 2026 | cat", "cal -- 03 -2026 | cat")
# any white space in front of a number is skipped: space, tab, newline, vertical tab, form feed, carriage return
case(*["echo -n -e '%s2026\\0' | xargs -0 cal | head -n 1" % c for c in ("\\t", "\\n", "\\x0b", "\\x0c", "\\r", " ", "  ", "+", "\\t+")],
     *["echo -n -e '3\\0%s2026\\0' | xargs -0 cal | head -n 1" % c for c in ("\\t", "\\n", "\\x0b", "\\x0c", "\\r", " ")])
# after -- everything is an argument, whatever it looks like
case("cal -- 3 -y | cat", "cal -- -y | cat", "cal -- -3 2026 | cat", "cal -3 -- 3 2026", "cal -y -- 2026", "cal -- -y 2026 | cat", "cal -- -1 | cat", "cal -- 1 -3 | cat", "cal -- 3 2026 | cat")
# -1 and -3: three months in the middle of the one asked for
case(*["cal -3 %d 2026" % m for m in range(1, 13)])
case(*["cal -3 %d 1752" % m for m in range(1, 13)])
case("cal -3 1 1", "cal -3 2 1", "cal -3 12 9999", "cal -3 11 9999", "cal -3 1 9999", "cal -3 12 1751", "cal -3 1 1753", "cal -3 9 1582", "cal -3 10 1582",
     "cal -3 12 1699", "cal -3 1 1700", "cal -3 2 1700", "cal -3 3 1700", "cal -3 2 1900", "cal -3 2 2024", "cal -3 3 2024")
case("cal -1 3 2026", "cal -1 mar 2026", "cal -1 9 1752", "cal -3 mar 2026", "cal -3 Sep 1752", "cal -31 3 2026", "cal -13 3 2026 | wc -l", "cal -3 3 2026 | wc -l",
     "cal -3 2 2026 | wc -l", "cal -3 3 2026 | tail -n 2", "cal -3 2 2026 | head -n 2", "cal -3 5 2026 | head -n 2", "cal -3 8 2026 | head -n 2", "cal -3 11 2026 | head -n 2",
     "cal -3 12 2026 | head -n 1", "cal -3 1 2026 | head -n 1")
# the ways of writing a month and a year
case("cal mar 2026", "cal MAR 2026", "cal Mar 2026", "cal march 2026", "cal MARCH 2026", "cal marc 2026", "cal marchh 2026", "cal marxyz 2026", "cal sep 1752", "cal sept 1752",
     "cal Sept 1752", "cal Sepxx 1752", "cal dec 2026", "cal Dec 2026", "cal decem 2026", "cal jan 2026", "cal feb 2024", "cal apr 2026", "cal may 2026", "cal maybe 2026",
     "cal jun 2026", "cal jul 2026", "cal aug 2026", "cal oct 2026", "cal nov 2026", "cal janu 2026", "cal junx 2026", "cal julx 2026")
case("cal 03 2026", "cal 003 2026", "cal +3 2026", "cal ' 3' 2026", "cal 3 02026", "cal 3 +2026", "cal 3 ' 2026'", "cal 03 0002", "cal 3 0000002", "cal 3 0001", "cal 12 09999",
     "cal 3 -- 2026 | cat", "cal -- 3 2026", "cal -- 2026 | head -n 2", "cal 1 +1", "cal ' 1' ' 1'", "cal 007 2026", "cal 0003 +0002026")
# errors: the message, not the status (cat takes its place)
case("cal 13 2026 | cat", "cal 0 2026 | cat", "cal 00 2026 | cat", "cal 14 2026 | cat", "cal 99 2026 | cat", "cal abc 2026 | cat", "cal ma 2026 | cat", "cal ju 2026 | cat", "cal m 2026 | cat",
     "cal 3rd 2026 | cat", "cal '3 ' 2026 | cat", "cal '' 2026 | cat", "cal '+' 2026 | cat", "cal ++3 2026 | cat", "cal 99999999999 3 | cat", "cal marx- 2026 | cat", "cal xmar 2026 | cat")
case("cal 3 0 | cat", "cal 3 10000 | cat", "cal 3 99999 | cat", "cal 3 99999999999 | cat", "cal 3 abc | cat", "cal 3 2026x | cat", "cal 3 '2026 ' | cat", "cal 3 '' | cat", "cal 3 + | cat",
     "cal 3 ++5 | cat", "cal 3 0000 | cat", "cal 3 00 | cat", "cal 3 10001 | cat", "cal 3 1e3 | cat", "cal 3 0x10 | cat", "cal 3 12.5 | cat", "cal 3 1,000 | cat")
case("cal 0 | cat", "cal 10000 | cat", "cal 00 | cat", "cal abc | cat", "cal mar | cat", "cal '' | cat", "cal + | cat", "cal 2026x | cat", "cal 99999999999 | cat", "cal 1e3 | cat", "cal 20 26 | cat")
case("cal 13 abc | cat", "cal abc abc | cat", "cal 13 0 | cat", "cal abc 0 | cat", "cal 13 99999 | cat", "cal abc 99999 | cat", "cal 0 abc | cat", "cal xx 10000 | cat")
case("cal -3 2026 | cat", "cal -1 2026 | cat", "cal -3 abc | cat", "cal -1 abc | cat", "cal -y abc | cat", "cal -3 13 2026 | cat", "cal -3 0 | cat", "cal -1 10000 | cat",
     "cal -3 mar 0 | cat", "cal -3 13 0 | cat", "cal -y 13 2026 | cat", "cal -y 3 0 | cat", "cal -y 0 | cat", "cal -y mar | cat", "cal -1 0 | cat", "cal -y 3 abc | cat")

# ---------------------------------------------------------------- split
# (the numbers of the options and the names of the files are checked one by one against
# GNU in tests/test_spcore.c; these are the program: what goes into the pieces, the
# order of the options, the messages. Pieces are listed with echo and a wildcard (no ls: it has
# its own layout) and read with head, which names each file. A usage error is status 1 for GNU and 2 here, so those cases
# end in | cat. Options only GNU has are in qemu_test.py.)
LS = "echo * | tr ' ' '\\n'"
PIECES = "head -n 100 x*"
S25 = "seq 1 25 > f"
case(S25, "split f", LS, PIECES)
case(S25, "split -l 10 f", LS, PIECES, "wc -l x* | sed 's/^ *//' | tr -s ' '")
case(S25, "split -l 5 f", LS, "wc -l x* | sed 's/^ *//' | tr -s ' '", "cat x* | cmp - f")
case(S25, "split -l 25 f", LS, "split -l 26 f pz", "echo pz* | tr ' ' '\\n'", "split -l 24 f py", "wc -l py* | sed 's/^ *//' | tr -s ' '", "split -l 1000 f pw", "wc -l pw* | sed 's/^ *//' | tr -s ' '")
case("seq 1 3 > f", "split -l 1 f", LS, PIECES, "split -l 2 f q", "head -n 100 q*")
case(S25, "split --lines=7 f", "wc -l x* | sed 's/^ *//' | tr -s ' '", "split --lines 8 f a", "wc -l a* | sed 's/^ *//' | tr -s ' '", "split -l7 f b", "wc -l b* | sed 's/^ *//' | tr -s ' '", "split -l 007 f c", "wc -l c* | sed 's/^ *//' | tr -s ' '", "split -l +9 f d", "wc -l d* | sed 's/^ *//' | tr -s ' '",
     "split -l ' 6' f e", "wc -l e* | sed 's/^ *//' | tr -s ' '", "split --lines=11 f g", "wc -l g* | sed 's/^ *//' | tr -s ' '", "split --lines=5 f h", "echo h* | tr ' ' '\\n'")
case(S25, "split -5 f", "wc -l x* | sed 's/^ *//' | tr -s ' '", "split -12 f a", "wc -l a* | sed 's/^ *//' | tr -s ' '", "split -3 f b", "echo b* | tr ' ' '\\n'", "split -1 -2 f c", "wc -l c* | sed 's/^ *//' | tr -s ' '", "split -5 -5 f d", "wc -l d* | sed 's/^ *//' | tr -s ' '", "split -007 f e", "wc -l e* | sed 's/^ *//' | tr -s ' '",
     "split -d5 f g", "wc -l g* | sed 's/^ *//' | tr -s ' '", "split -x3 f h", "echo h* | tr ' ' '\\n'", "split -1x2 f i", "wc -l i* | sed 's/^ *//' | tr -s ' '")
case(S25, "split -1 -d -2 f", "wc -l x* | sed 's/^ *//' | tr -s ' '", "split -123456789012345678901 f p", "wc -l p* | sed 's/^ *//' | tr -s ' '", "split -3 -- f q", "echo q* | tr ' ' '\\n'", "split -l 5 -- f r", "echo r* | tr ' ' '\\n'")
case(S25, "cat f | split -l 10", LS, PIECES, "cat f | split -l 10 - pre", "echo pre* | tr ' ' '\\n'", "split -l 10 - pr < f", "echo pr* | tr ' ' '\\n'")
case(S25, "mkdir sub", "split -l 10 f sub/", "echo sub/* | tr ' ' '\\n'", "split -l 10 f sub/p", "echo sub/* | tr ' ' '\\n'", "split -l 10 f ./q", "echo * | tr ' ' '\\n'", "split -l 10 f -", "echo * | tr ' ' '\\n'")
case("echo -n > e", "split e", LS, "split -l 3 e", "split -b 3 e", "split -d e", LS, "echo -n | split", LS)
case("echo -n -e '1\\n2\\n3' > g", "split -l 2 g", LS, PIECES, "wc -c xaa xab | sed 's/^ *//' | tr -s ' '", "split -l 1 g q", "wc -c q* | sed 's/^ *//' | tr -s ' '", "split -l 3 g r", "wc -c r* | sed 's/^ *//' | tr -s ' '", "split -l 4 g s", "wc -c s* | sed 's/^ *//' | tr -s ' '")
case("echo -e '\\n\\n\\n\\n\\n' > g", "split -l 2 g", LS, "wc -c x* | sed 's/^ *//' | tr -s ' '", "echo -n -e '\\n\\n\\n' > h", "split -l 1 h q", "wc -c q* | sed 's/^ *//' | tr -s ' '")
case("echo -n -e 'a\\0b\\nc\\0d\\ne\\n' > g", "split -l 1 g", "od -c xaa", "od -c xab", "od -c xac", "split -l 2 g q", "od -c qaa", "od -c qab")
case("echo -n -e 'a\\r\\nb\\r\\nc\\n' > g", "split -l 2 g", "od -c xaa", "od -c xab")
# bytes
B26 = "echo -n abcdefghijklmnopqrstuvwxyz > f"
case(B26, "split -b 5 f", LS, PIECES, "split -b 13 f a", "head -n 100 a*", "split -b 26 f b", "head -n 100 b*", "split -b 27 f c", "head -n 100 c*", "split -b 1 f d", "echo d* | tr ' ' '\\n'", "head -n 100 da* | head -n 8")
case(B26, "split --bytes=5 f", "wc -c x* | sed 's/^ *//' | tr -s ' '", "split --bytes 6 f a", "wc -c a* | sed 's/^ *//' | tr -s ' '", "split -b5 f b", "wc -c b* | sed 's/^ *//' | tr -s ' '", "split -b 007 f c", "wc -c c* | sed 's/^ *//' | tr -s ' '", "split -b +9 f d", "wc -c d* | sed 's/^ *//' | tr -s ' '", "split -b ' 4' f e", "wc -c e* | sed 's/^ *//' | tr -s ' '",
     "split --by=12 f g", "wc -c g* | sed 's/^ *//' | tr -s ' '", "split --bytes=5 -- f h", "echo h* | tr ' ' '\\n'")
case(B26, "split -b 1K f", "wc -c x* | sed 's/^ *//' | tr -s ' '", "split -b 1b f a", "wc -c a* | sed 's/^ *//' | tr -s ' '", "split -b K f b", "wc -c b* | sed 's/^ *//' | tr -s ' '", "split -b 1k f c", "wc -c c* | sed 's/^ *//' | tr -s ' '", "split -b 1KB f d", "wc -c d* | sed 's/^ *//' | tr -s ' '", "split -b 1KiB f e", "wc -c e* | sed 's/^ *//' | tr -s ' '",
     "split -b 1M f g", "wc -c g* | sed 's/^ *//' | tr -s ' '", "split -b 1G f h", "wc -c h* | sed 's/^ *//' | tr -s ' '", "split -b 1MB f i", "wc -c i* | sed 's/^ *//' | tr -s ' '", "split -b M f j", "wc -c j* | sed 's/^ *//' | tr -s ' '", "split -b b f k", "wc -c k* | sed 's/^ *//' | tr -s ' '", "split -b KB f l", "wc -c l* | sed 's/^ *//' | tr -s ' '",
     "split -b 4Q f m", "wc -c m* | sed 's/^ *//' | tr -s ' '", "split -b 18446744073709551615 f n", "wc -c n* | sed 's/^ *//' | tr -s ' '", "split -b 99999999999999999999 f o", "wc -c o* | sed 's/^ *//' | tr -s ' '", "split -b 1QiB f p", "wc -c p* | sed 's/^ *//' | tr -s ' '")
case("seq 1 10 > f", "split -b 7 f", PIECES, "split -b 3 f q", "head -n 100 q*", "split -b 20 f r", "wc -c r* | sed 's/^ *//' | tr -s ' '")
case("yes abcdefg | head -c 10000 > big", "split -b 4096 big", "wc -c x* | sed 's/^ *//' | tr -s ' '", "cat x* | cmp - big", "split -b 4095 big a", "wc -c a* | sed 's/^ *//' | tr -s ' '", "split -b 4097 big b", "wc -c b* | sed 's/^ *//' | tr -s ' '", "split -b 1000 big c", "echo c* | tr ' ' '\\n'",
     "cat c* | cmp - big", "split -b 3333 big d", "wc -c d* | sed 's/^ *//' | tr -s ' '", "cat d* | cmp - big")
case("yes abcdefg | head -n 2500 > big", "split big", "wc -l x* | sed 's/^ *//' | tr -s ' '", "cat x* | cmp - big", "split -l 999 big a", "wc -l a* | sed 's/^ *//' | tr -s ' '", "seq 1 60 > s", "split -l 1 s zz", "echo zz* | tr ' ' '\\n' | wc -l", "cat zz* | cmp - s",
     "split -l 500 big c", "wc -l c* | sed 's/^ *//' | tr -s ' '", "split -l 4096 big d", "wc -l d* | sed 's/^ *//' | tr -s ' '")
# the names
case("seq 1 26 > f", "split -l 1 -a 1 f", LS, "split -l 1 -a 3 f a", "echo a* | tr ' ' '\\n' | head -n 3", "split -l 1 -a 0 f b", "echo b* | tr ' ' '\\n' | head -n 3", "split -l 1 -a +3 f c", "echo c* | tr ' ' '\\n' | head -n 2", "split -l 1 -a 03 f d",
     "echo d* | tr ' ' '\\n' | head -n 2", "split -l 1 --suffix-length=4 f e", "echo e* | tr ' ' '\\n' | head -n 2", "split -l 1 --suffix-length 2 f g", "echo g* | tr ' ' '\\n' | head -n 2", "split -l 1 -a2 f h", "echo h* | tr ' ' '\\n' | head -n 2")
case("seq 1 30 > f", "?split -l 1 -a 1 f", LS, "?split -l 1 -a 1 f q", "echo q* | tr ' ' '\\n' | wc -l", "?split -l 1 -d -a 1 f d", "echo d* | tr ' ' '\\n'", "?split -l 1 -x -a 1 f h", "echo h* | tr ' ' '\\n' | wc -l")
case("seq 1 26 > f", "split -l 1 -a 1 f", "ls | wc -l", "seq 1 10 > g", "split -l 1 -d -a 1 g d", "echo d* | tr ' ' '\\n' | wc -l", "seq 1 16 > h", "split -l 1 -x -a 1 h q", "echo q* | tr ' ' '\\n' | wc -l")
case("seq 1 660 > f", "split -l 1 f", "ls | wc -l", "cat xaa xab xyz", "cat xzaaa xzaaj", "?cat xzaak", "?cat xzaaz", "?cat xzaba", "?cat xzzzzz")
case("seq 1 30 > f", "split -l 1 -d f", "echo x* | tr ' ' '\\n' | head -n 3", "echo x* | tr ' ' '\\n' | tail -n 2", "split -l 1 -x f a", "echo a* | tr ' ' '\\n' | head -n 12", "echo a* | tr ' ' '\\n' | tail -n 2", "split -l 1 --numeric-suffixes f b", "echo b* | tr ' ' '\\n' | tail -n 2",
     "split -l 1 --hex-suffixes f c", "echo c* | tr ' ' '\\n' | tail -n 2", "split -l 1 -d -a 3 f d", "echo d* | tr ' ' '\\n' | head -n 2", "split -l 1 -x -a 3 f e", "echo e* | tr ' ' '\\n' | tail -n 2")
case("seq 1 20 > f", "split -l 1 -d -x f", "echo x* | tr ' ' '\\n' | tail -n 6", "split -l 1 -x -d f a", "echo a* | tr ' ' '\\n' | tail -n 6", "split -l 1 --hex-suffixes --numeric-suffixes f b", "echo b* | tr ' ' '\\n' | tail -n 3",
     "split -l 1 --numeric-suffixes --hex-suffixes f c", "echo c* | tr ' ' '\\n' | tail -n 3")
case("seq 1 100 > f", "split -l 1 -d f", "cat x88 x89 x9000 x9001 x9009", "?cat x9010", "ls | wc -l", "split -l 1 -x f a", "cat a09 a0a a0f a10 a63", "ls | wc -l")
case("seq 1 12 > f", "split -l 1 --numeric-suffixes=5 f", "echo x* | tr ' ' '\\n'", "split -l 1 --numeric-suffixes=08 f a", "echo a* | tr ' ' '\\n'", "split -l 1 -d --numeric-suffixes=005 -a 3 f b", "echo b* | tr ' ' '\\n' | head -n 3",
     "split -l 1 --numeric-suffixes=0 f c", "echo c* | tr ' ' '\\n' | head -n 2", "split -l 1 --numeric-suffixes=0 -a 0 f d", "echo d* | tr ' ' '\\n' | head -n 2", "split -l 1 --numeric-suffixes= f e", "echo e* | tr ' ' '\\n' | head -n 2",
     "split -l 1 --numeric-suffixes=7 -a 4 f g", "echo g* | tr ' ' '\\n' | head -n 2", "split -l 1 --numeric-s=3 f h", "echo h* | tr ' ' '\\n' | head -n 2")
case("seq 1 12 > f", "split -l 1 --hex-suffixes=5 f", "echo x* | tr ' ' '\\n'", "split -l 1 --hex-suffixes=09 f a", "echo a* | tr ' ' '\\n'", "split -l 1 -x --hex-suffixes=009 -a 3 f b", "echo b* | tr ' ' '\\n' | head -n 3",
     "split -l 1 --hex-suffixes=0 f c", "echo c* | tr ' ' '\\n' | head -n 2", "split -l 1 --hex-suffixes=099 -a 3 f d", "echo d* | tr ' ' '\\n' | head -n 2", "split -l 1 --hex-suffixes= f e", "echo e* | tr ' ' '\\n' | head -n 2",
     "split -l 1 --hex-suffixes=19 f g", "echo g* | tr ' ' '\\n' | head -n 1", "split -l 1 --hex-su=1 f h", "echo h* | tr ' ' '\\n' | head -n 2")
case("seq 1 30 > f", "?split -l 1 --numeric-suffixes=85 f", "echo x* | tr ' ' '\\n'", "?split -l 1 --numeric-suffixes=95 f p", "echo p* | tr ' ' '\\n'", "?split -l 1 --numeric-suffixes=95 -a 0 f q", "echo q* | tr ' ' '\\n'",
     "?split -l 1 -d --numeric-suffixes=95 f r", "echo r* | tr ' ' '\\n'", "?split -l 1 --hex-suffixes=95 f s", "echo s* | tr ' ' '\\n' | wc -l")
case("seq 1 5 > f", "split -l 1 --additional-suffix=.txt f", LS, "split -l 1 --additional-suffix .log f a", "echo a* | tr ' ' '\\n'", "split -l 1 --additional-suffix= f b", "echo b* | tr ' ' '\\n' | head -n 2",
     "split -l 1 -d --additional-suffix=.d f c", "echo c* | tr ' ' '\\n' | head -n 2", "split -l 1 --additional-suffix=.q -a 3 f d", "echo d* | tr ' ' '\\n' | head -n 2", "split -l 1 --add=.z f e", "echo e* | tr ' ' '\\n' | head -n 2",
     "split -l 1 --additional-suffix=-x f g", "echo g* | tr ' ' '\\n' | head -n 2", "cat xaa.txt")
case("seq 1 25 > f", "split -l 10 --verbose f", "split -l 10 --verbose f pre", "split -l 10 -d --verbose f d", "split --verbose -b 20 f b", "split -l 10 --verb --additional-suffix=.z f z", "split --verb - < f | wc -l",
     "echo -n > e", "split --verbose e", "split --verbose -l 100 f big")
# options mixed with operands, long options by prefix
case(S25, "split f -l 10", LS, "split f p -l 10", "echo p* | tr ' ' '\\n'", "split -l 10 f p -a 3", "echo p* | tr ' ' '\\n' | head -n 2", "split f -d p", "echo p* | tr ' ' '\\n' | head -n 2", "split -d f q -l 7", "wc -l q* | sed 's/^ *//' | tr -s ' '")
case(S25, "split --lines 10 f", "ls | wc -l", "split --lines=10 --suffix-length=3 f p", "echo p* | tr ' ' '\\n' | head -n 2", "split --by=40 --suffix-length 3 f q", "echo q* | tr ' ' '\\n' | head -n 2", "split --b=50 --ad=.t --a=1 --verb f r")
# numbers that are no good
case(S25, "?split -l 0 f", "?split -l x f", "?split -l -1 f", "?split -l 1K f", "?split -l '' f", "?split -l 1 f -", "?split -l 0x10 f", "?split -l 1e3 f", "?split -l ' 5 ' f", "?split -l 5k f",
     "?split -l 1.5 f", "?split --lines=0 f", "?split --lines=abc f", "?split -l 00 f", "?split -l + f", "?split -l - f", LS)
case(S25, "?split -b 0 f", "?split -b x f", "?split -b -1 f", "?split -b 1c f", "?split -b '' f", "?split -b 1w f", "?split -b 1g f", "?split -b B f", "?split -b Ki f", "?split -b KK f", "?split -b 1bB f",
     "?split -b '1K ' f", "?split -b 0K f", "?split -b 0b f", "?split --bytes=0 f", "?split --bytes=abc f", "?split -b 0x10 f", "?split -b iB f", "?split -b 1KiBx f", "?split -b ' K' f", "?split -b +K f", LS)
case(S25, "?split -0 f", "?split -00 f", "?split -l5d f", "?split -a x f", "?split -a -1 f", "?split -a 5x f", "?split -a '' f", "?split -a 1k f", "?split -a -5x f", "?split -a 99999999999999999999 f",
     "?split -a -99999999999999999999 f", "?split --suffix-length=x f", "?split -a -2000000000 f", LS)
case(S25, "?split -l 5 -l 5 f | cat", "?split -l 5 -b 5 f | cat", "?split -b 5 -l 5 f | cat", "?split -5 -l 3 f | cat", "?split -l 3 -5 f | cat", "?split -b5 -3 f | cat", "?split -3 -b5 f | cat",
     "?split -5l 3 f | cat", "?split --lines=3 --bytes=3 f | cat", LS)
case(S25, "?split -z f | cat", "?split --foo f | cat", "?split -l | cat", "?split --lines | cat", "?split -b | cat", "?split -a | cat", "?split --verbose=1 f | cat", "?split --additional-suffix | cat",
     "?split --help=1 | cat", "?split -ld 5 f | cat", "?split -dz f | cat", "?split --q f | cat", "?split --l 5 f | cat", "?split --lin 5 f | cat", "?split --n f | cat", "?split --nu f | cat",
     "?split --h | cat", "?split --s f | cat", "?split --v f | cat", "?split --e f | cat", "?split --su f | cat", "?split --numeric-suffixes=1 --foo | cat")
case(S25, "?split f p x | cat", "?split f p x y | cat", "?split -l 3 f p x | cat", "?split -- f p x | cat", "?split f p -- x | cat", LS)
case(S25, "?split --additional-suffix=a/b f | cat", "?split --additional-suffix=/ f | cat", "?split --additional-suffix=a/ f | cat", "?split -l 3 --additional-suffix ./x f | cat", LS)
case(S25, "?split --numeric-suffixes=100 f | cat", "?split --numeric-suffixes=x f | cat", "?split --numeric-suffixes=+5 f | cat", "?split --numeric-suffixes=' 5' f | cat", "?split --numeric-suffixes='5 ' f | cat",
     "?split --numeric-suffixes=0x5 f | cat", "?split --numeric-suffixes=-1 f | cat", "?split --numeric-suffixes=1e2 f | cat", "?split --numeric-suffixes=5a f | cat",
     "?split --numeric-suffixes=ff f | cat", "?split --numeric-suffixes=99999999999999999999 f | cat", "?split -a 3 --numeric-suffixes=1000 f | cat", "?split -a 1 --numeric-suffixes=10 f | cat", LS)
case(S25, "?split --hex-suffixes=100 f | cat", "?split --hex-suffixes=x f | cat", "?split --hex-suffixes=+5 f | cat", "?split --hex-suffixes=F f | cat", "?split --hex-suffixes=0F f | cat",
     "?split --hex-suffixes=0x5 f | cat", "?split --hex-suffixes=1g f | cat", "?split -a 3 --hex-suffixes=1000 f | cat", "?split -a 1 --hex-suffixes=10 f | cat", "?split --hex-suffixes=-1 f | cat",
     "?split -d --hex-suffixes=ff f | cat", "?split -x --numeric-suffixes=ff f | cat", "?split --numeric-suffixes=5 -x f p", "echo p* | tr ' ' '\\n' | head -n 2", LS)
# files
case(S25, "?split nosuch", "?split -l 2 nosuch pre", "mkdir d", "?split d", "?split -l 2 d pre", "?split f nodir/x", "?split f d/x", "echo d/* | tr ' ' '\\n'", "?split --numeric-suffixes 5 f", LS)
case("seq 1 30 > xaa", "?split -l 10 xaa", "echo * | tr ' ' '\\n'", "seq 1 30 > f", "split -l 10 f", "echo * | tr ' ' '\\n'", "?split -l 10 xab", "cat xaa | head -n 2", "?split -l 10 xab pre", "echo * | tr ' ' '\\n'", "?split -l 10 f xa", "echo * | tr ' ' '\\n'")
case(S25, "split -l 10 f", "split -l 3 f", "wc -l x* | sed 's/^ *//' | tr -s ' '", "split -b 20 f", "wc -c x* | sed 's/^ *//' | tr -s ' '")

# ---------------------------------------------------------------- join
# (field numbers, -o items, the cutting of lines and the comparing of keys are checked one by one
# against GNU in tests/test_jncore.c; these are the program: the pairing, the unpaired lines, the
# order of the output, the option order tricks, the order check, the messages. A usage error is
# status 1 for GNU and 2 here, so those cases end in | cat. --help and --version are in
# qemu_test.py. A file name with a NUL cannot be typed, and -z output is read through tr.)
JA = "echo -e 'a 1\\nb 2\\nc 3\\nd 4' > f1"
JB = "echo -e 'a x\\nc y\\ne z' > f2"
case(JA, JB, "join f1 f2", "join f2 f1", "join -a 1 f1 f2", "join -a 2 f1 f2", "join -a 1 -a 2 f1 f2", "join -a1 -a2 f1 f2", "join -v 1 f1 f2", "join -v 2 f1 f2", "join -v1 -v2 f1 f2", "join -v 1 -a 2 f1 f2",
     "join -a 2 -v 1 f1 f2", "join -v2 -a1 f1 f2")
case(JA, JB, "join -e X -a 1 -a 2 f1 f2", "join -e X -o 0,1.2,2.2 -a 1 -a 2 f1 f2", "join -o 2.2,0,1.2 f1 f2", "join -o 0 f1 f2", "join -o 1.1 f1 f2", "join -o 2.1,2.1,1.2,1.2 f1 f2",
     "join -o 1.1,1.2,1.3,2.5 f1 f2", "join -e '' -o 1.1,1.2,1.3 -a1 f1 f2", "join -e - -o 1.3,0 -a1 -a2 f1 f2", "join -o '1.2 2.2' f1 f2", "join -o 1.2,2.2 -o 0 f1 f2")
case(JA, JB, "join -o 1.2 2.2 f1 f2", "join -o 1.2 f1 f2", "join -o 1.2 -a 1 f1 f2", "join f1 -o 1.2 f2", "join f1 f2 -o 1.2", "join f1 -o 1.2 2.2 f2", "join -o 0 -o 1.2 f1 f2", "join -o 1.2,0 2.2 f1 f2",
     "join -a 1 f1 -o 1.2 f2", "join -o 2.1 -j 1 f1 f2")
case(JA, JB, "join -o auto f1 f2", "join -o auto -a1 -a2 f1 f2", "join -o auto -e X -a1 -a2 f1 f2", "echo -e 'a 1 2 3\\nb 5' > g1", "echo -e 'a x\\nb y z' > g2", "join -o auto g1 g2", "join -o auto -e - -a1 -a2 g1 g2",
     "join -o auto g2 g1", "join -o auto -e - -a1 -a2 g2 g1", "join g1 g2", "join -e - g1 g2", "join -a1 -e - g1 g2", "join -o auto -o 0 g1 g2", "join -o 1.2 -o auto g1 g2")
case("echo -e 'a 1 2 3' > g1", "echo -e 'a x y' > g2", "echo -n > e", "join -o auto g1 e", "join -o auto -a1 g1 e", "join -o auto -a1 -e X g1 e", "join -o auto -a2 -e X g1 e", "join -o auto -a2 -e X e g2",
     "join -o auto -a1 -a2 -e X e e", "join -a1 -a2 e e", "join -a1 -a2 g1 e", "join -a1 -a2 e g2", "join -v1 g1 e", "join -v2 e g2", "join g1 e", "join e g2", "join e e")
# keys in other places
case("echo -e '1 a\\n2 b\\n3 c' > f1", "echo -e 'x 1\\ny 2\\nz 4' > f2", "join -1 1 -2 2 f1 f2", "join -1 2 -2 1 f1 f2", "join -2 2 f1 f2", "join -1 1 -2 2 -a 1 -a 2 f1 f2", "join -j 1 f1 f2", "join -j 2 f1 f2", "join -j1 2 f1 f2", "join -j2 2 f1 f2",
     "join -2 2 -a1 f1 f2", "join -j 3 f1 f2", "join -j 3 -a 1 f1 f2", "join -j 3 -a 1 -a 2 -e X f1 f2", "join -o 0,1.1,2.1 -2 2 f1 f2", "join -1 3 -2 3 f1 f2", "join -v 1 -1 2 -2 1 f1 f2", "join -1 2 -2 2 f1 f2")
case("echo -e 'a b c\\nd e f' > g1", "echo -e 'x b z\\ny e w' > g2", "join -j 2 g1 g2", "join -j2 2 g1 g2", "join -j1 2 g1 g2", "join -j1 g1 g2", "join -j2 g1 g2", "join -j1 -- g1 g2", "join -j 1 -j 1 g1 g2", "join -1 2 -2 2 -j 2 g1 g2",
     "join -j 2 -1 2 g1 g2", "join -j 2 -2 2 g1 g2", "join -j1 2 -j 2 g1 g2", "join -j 2 -j1 2 g1 g2", "join -j1 2 -j2 2 g1 g2", "join -ij 2 g1 g2", "?join -ij2 2 g1 g2 | cat", "?join -jj g1 g2", "?join -j1x g1 g2", "join g1 -j1 2 g2", "?join g1 g2 -j1 2 | cat")
# groups: every line with the same key pairs with every line of the other file
case("echo -e 'a 1\\na 2\\nb 3\\nb 4\\nb 5\\nc 6' > f1", "echo -e 'a x\\na y\\nb z\\nd w' > f2", "join f1 f2", "join -a 1 f1 f2", "join -a 2 f1 f2", "join -a1 -a2 f1 f2", "join -v 1 f1 f2", "join -v 2 f1 f2", "join f2 f1", "join -o 1.2,2.2 f1 f2",
     "join -v1 -v2 f1 f2", "join -e - -a1 -a2 -o 0,1.2,2.2 f1 f2")
case("echo -e 'a 1\\na 2\\na 3' > f1", "echo -e 'a x\\na y\\na z' > f2", "join f1 f2", "join f1 f2 | wc -l", "join -v1 f1 f2", "join -a1 -a2 f1 f2 | wc -l", "join f1 f1", "join -o 1.2,2.2 f1 f2 | sort | uniq -c | sed 's/^ *//'")
case("echo -e 'a 1\\na 2' > f1", "echo -e 'a x' > f2", "join f1 f2", "join f2 f1", "join -a1 f1 f2", "join -a2 f1 f2", "echo -e 'a 1\\nb 2\\nb 3' > f3", "echo -e 'b x\\nb y\\nc z' > f4", "join f3 f4", "join -a1 -a2 f3 f4", "join f4 f3", "join -v1 f3 f4", "join -v2 f3 f4")
# blanks
case("echo -e 'a  b\\t c' > f1", "echo -e '  a x\\t\\ty' > f2", "join f1 f2", "join -o 0,1.2,1.3,2.2,2.3 f1 f2", "echo -e 'a b  ' > h1", "echo -e 'a c ' > h2", "join h1 h2", "join -o 0,1.2,1.3,2.2,2.3 -e - h1 h2", "join -a1 -a2 -e - h1 h2",
     "echo -e ' \\t ' > b1", "echo -e 'a b' > b2", "join b1 b2", "join -a1 -a2 b1 b2", "join -a1 -a2 -e - b1 b2", "join -j 2 b2 b2", "join -j 2 -o 0,1.1,2.1 b2 b2", "join -j 3 b2 b2", "join -j 3 -o 0,1.1,2.1 -e - b2 b2")
case("echo -e 'a\\nb\\n\\nc' > f1", "echo -e 'a\\n\\nc\\nd' > f2", "?join f1 f2", "?join -a1 -a2 f1 f2", "?join -a1 -a2 -e - f1 f2", "join --nocheck-order -a1 -a2 f1 f2", "?join -v1 -v2 f1 f2", "?join --check-order f1 f2")
case("echo -e '\\n\\n' > f1", "echo -e 'a\\n\\n' > f2", "?join f1 f2", "?join -a1 f1 f2", "?join -a1 -e - f1 f2", "?join -a1 -a2 -e - f1 f2", "?join -v1 f1 f2", "?join -v2 f1 f2", "?join -j 2 f1 f2", "?join -j 2 -a 1 -e - f1 f2")
# -t
case("echo -e 'a:1:x\\nb:2:y\\nc::z' > f1", "echo -e 'a:p\\nc:q\\nd:r' > f2", "join -t : f1 f2", "join -t: f1 f2", "join -t ':' -a1 -a2 f1 f2", "join -t : -o 0,1.2,1.3,2.2 -a1 -a2 -e - f1 f2", "?join -t : -1 2 -2 1 f1 f2", "join -t : -j 3 f1 f2",
     "?join -t : -j 2 -a 1 f1 f2", "?join -t : -j 2 -a 1 -e '?' f1 f2", "join -t ':' -t ':' f1 f2", "join -t : -o auto -a1 -a2 -e - f1 f2", "join -t , f1 f2", "join -t , -a1 -a2 f1 f2", "join -t ' ' f1 f2", "join -t = -a1 f1 f2")
case("echo -e 'a b:1\\nc d:2' > f1", "echo -e 'a b:x\\nc  d:y\\ne:z' > f2", "join -t : f1 f2", "join -t : -a1 -a2 f1 f2", "join -t ' ' f1 f2", "join -t ' ' -a1 -a2 f1 f2", "join f1 f2", "join -t a f1 f2")
case("echo -e 'a\\tb\\tc\\nd\\te\\tf' > f1", "echo -e 'a\\tq\\nd\\tr\\tw' > f2", "join f1 f2", "join -j 2 f1 f2", "join -j 2 -a1 -a2 f1 f2 | od -c | head -n 5")
case("echo -e 'a b\\nc d e\\nc d f\\nx' > f1", "echo -e 'a b\\nc d e\\nz' > f2", "join -t '' f1 f2", "join -t '' -a1 -a2 f1 f2", "join -t '' -v1 -v2 f1 f2", "join -t '' -o 0 f1 f2", "join -t '' -o 1.1,2.1 f1 f2", "join -t '' -o 1.2 -e - f1 f2",
     "join -t '' -a1 -e X -o 1.2 f1 f2", "join -t '' -j 2 f1 f2", "join -t '' -j 2 -a1 -a2 f1 f2", "join -t '' -o auto f1 f2", "join -t '' -i f1 f2", "join -t '' --nocheck-order -a1 -a2 f1 f2")
case("echo -e 'a\\0b\\0c' > f1", "echo -e 'a\\0x\\0y' > f2", "join -t '\\0' f1 f2 | od -c", "join -t '\\0' -a1 -a2 -j 2 f1 f2 | od -c", "join -t '\\0' -o 0,1.3,2.3 f1 f2 | od -c", "join -t '\\0' -t '\\0' f1 f2 | od -c", "?join -t '\\0' -t ' ' f1 f2", "?join -t '\\0' -t '' f1 f2")
case("echo -e 'a,b' > f1", "?join -t ab f1 f1", "?join -t '' -t ' ' f1 f1", "?join -t ' ' -t '' f1 f1", "?join -t '\\t' f1 f1", "?join -t '\\\\0' f1 f1", "?join -t '\\1' f1 f1", "?join -t '\\00' f1 f1", "join -t , f1 f1", "?join -t , -t , -t , f1 f1", "?join -t , -t ';' f1 f1")
# -i
case("echo -e 'a 1\\nB 2\\nc 3\\nD 4' > f1", "echo -e 'A x\\nb y\\nC z\\nE w' > f2", "join -i f1 f2", "?join f1 f2", "join -i -a1 -a2 f1 f2", "join --ignore-case f1 f2", "join --ignore f1 f2", "join --i -a1 f1 f2", "join -i -o 0,1.1,2.1 f1 f2",
     "join -ia1 -a2 f1 f2", "join -ii f1 f2", "join -i -v1 f1 f2", "echo -e 'Z 1\\n_ 2\\na 3' > f5", "echo -e 'A 1\\nz 2' > f6", "join -i --nocheck-order f5 f6", "join --nocheck-order f5 f6")
# --header
case("echo -e 'k v1\\na 1\\nb 2\\nc 3' > f1", "echo -e 'k v2\\na x\\nc y\\ne z' > f2", "join --header f1 f2", "join --header -a1 f1 f2", "join --header -a1 -a2 f1 f2", "join --header -v1 f1 f2", "join --header -o 0,1.2,2.2 f1 f2", "join f1 f2", "join --head f1 f2",
     "join --header -o auto f1 f2", "join --header -j 2 f1 f2", "join --header -t ' ' f1 f2", "join --header --check-order f1 f2")
case("echo -e 'h1 h2\\nb 2\\na 1' > f1", "echo -e 'x y\\na 1\\nb 2' > f2", "?join --header f1 f2", "?join --header --check-order f1 f2", "join --header --nocheck-order f1 f2", "?join --header --check-order f1 f2")
case("echo -e 'k v1\\na 1' > f1", "echo -n > e", "join --header f1 e", "join --header e f1", "join --header e e", "join --header -a1 f1 e", "join --header -a2 e f1", "join --header -v1 f1 e", "join --header -v2 e f1", "join --header -o auto f1 e",
     "echo -e 'only' > f3", "join --header f3 f1", "join --header f1 f3", "join --header -a1 -a2 f3 f1", "join --header -a1 -a2 f1 f3", "join --header f3 f3", "join --header -o 1.1,2.1 f3 f1", "join --header -j 2 f3 f1", "join -a1 -a2 --header e f3")
# the order check
case("echo -e 'b 1\\na 2\\nc 3\\nd 4' > f1", "echo -e 'a x\\nc y\\ne z' > f2", "?join f1 f2", "?join --check-order f1 f2", "join --nocheck-order f1 f2", "?join -a1 f1 f2", "?join -v1 f1 f2", "?join -a1 -a2 f2 f1", "?join f2 f1", "?join -i f1 f2")
case("echo -e 'a 1\\nc 2\\nb 3\\nd 4\\nz 9\\ny 8' > f1", "echo -e 'a x\\nb y\\nc z\\nd w\\nz q' > f2", "?join f1 f2", "?join --check-order f1 f2", "?join -a1 f1 f2", "?join -v1 f1 f2", "join --nocheck-order f1 f2", "?join f2 f1", "?join -v 1 f2 f1")
case("echo -e 'a 1\\nb 2\\nz 3\\nc 4\\nd 5' > f1", "echo -e 'a x\\nb y' > f2", "?join f1 f2", "?join --check-order f1 f2", "?join -a1 f1 f2", "join --nocheck-order -a1 f1 f2", "?join f2 f1", "?join -a2 f2 f1", "?join -v2 f2 f1")
case("echo -e 'a 1\\nb 2\\nz 3\\nc 4\\nd 5' > f1", "echo -e 'a x\\nb y' > f2", "echo -e 'a x\\nb y\\nq q' > f3", "?join f1 f3", "?join -a1 f1 f3", "?join f3 f1", "?join -v1 f3 f1")
case("echo -e 'a 1\\nb 2' > f1", "echo -e 'a x\\nc y\\nb z\\nd w\\ne v\\nd u' > f2", "?join f1 f2", "?join -a2 f1 f2", "?join --check-order f1 f2", "?join -a1 -a2 f1 f2", "?join f2 f1", "?join -a1 f2 f1", "?join -v1 f2 f1", "?join -v2 f2 f1")
case("echo -e 'c 1\\nb 2\\na 3' > f1", "echo -e 'c x\\nb y\\na z' > f2", "?join f1 f2", "?join --check-order f1 f2", "?join -a1 f1 f2", "join --nocheck-order f1 f2", "?join -i f1 f2", "?join -v1 -v2 f1 f2")
case("echo -e 'a 1\\nb 2\\nc 3\\nz 4\\ny 5\\nx 6' > f1", "echo -e 'a 1\\nb 2\\nc 3\\nz 4\\ny 5\\nx 6' > f2", "?join f1 f2", "?join -a1 f1 f2", "?join --check-order f1 f2", "join --nocheck-order f1 f2", "echo -e 'b 1\\na 2' > f3", "echo -e 'a 1\\nb 2' > f4", "?join f3 f4", "?join -a1 f3 f4", "?join -a2 f3 f4", "?join -v2 f3 f4")
case("echo -e 'a 1\\nB 2\\nc 3' > f1", "echo -e 'a 1\\nB 2\\nc 3' > f2", "join f1 f2", "?join -i f1 f2", "join -i --nocheck-order f1 f2", "echo -e 'a 1\\nb 2\\nc 3' > f3", "join -i f1 f3", "?join f1 f3")
case("echo -e 'a 3\\nb 2\\nc 1' > f1", "echo -e 'a 3\\nb 2\\nc 1' > f2", "join -1 2 -2 2 f1 f2", "?join -1 2 -2 2 -a1 f1 f2", "?join -j 2 -v1 f1 f2", "?join -j 2 -a1 -a2 f1 f2", "?join -j 2 --check-order f1 f2", "join -j 2 --nocheck-order f1 f2", "join f1 f2")
# input from standard input, files without a final newline, ends
case(JA, JB, "cat f1 | join - f2", "cat f2 | join f1 -", "join - f2 < f1", "join f1 - < f2", "?join - - < f1", "?join - - | cat", "cat f1 | join -a1 -e Z -o 0,1.2,2.2 - f2", "?join f1 f2 - | cat", "?join - f1 f2 | cat")
case("echo -n -e 'a 1\\nb 2' > f1", "echo -n -e 'a x\\nb y' > f2", "echo -n 'a 1' > f3", "echo -n 'a' > f4", "join f1 f2", "join -a1 f1 f2", "join f3 f4", "join f3 f3", "join -a1 -a2 f4 f3", "join -o 1.2 f3 f4", "join -o 0,1.1,1.2,2.1 -e - f3 f4", "cat f1 | join -a 2 - f2")
case("echo -n -e 'a 1\\nb' > f1", "echo -n -e 'a\\nb 2' > f2", "join f1 f2", "join -a1 -a2 f1 f2", "join -o auto f1 f2", "join -o auto -e - -a1 -a2 f1 f2")
case("echo -e 'a 1' > f1", "echo -e 'a 2' > f2", "echo -e 'a 3' > f3", "?join f1 f2 f3 | cat", "?join f1 f2 f3 f4 | cat", "?join -a 1 f1 f2 f3 | cat", "?join f1 | cat", "?join | cat", "?join -a 1 | cat", "?join -a1 f1 | cat", "?join f1 -a1 | cat",
     "?join f1 -a 1 | cat", "?join f1 f2 f3 -- f4 | cat", "?join -- f1 f2 f3 | cat", "?join -- f1 | cat", "?join f1 -- f2 | cat", "join -- f1 f2", "join f1 -- f2", "?join -a 1 -- f1 | cat", "?join -o 1.1 f1 | cat", "?join -j1 f1 | cat")
case("echo -e 'a 1' > f1", "?join -x f1 f1 | cat", "?join -a | cat", "?join f1 f1 -a | cat", "?join -ax f1 f1 | cat", "?join --foo f1 f1 | cat", "?join --foo=1 f1 f1 | cat", "?join --h f1 f1 | cat", "?join --he f1 f1 | cat", "?join --header=1 f1 f1 | cat",
     "?join --nocheck-order=1 f1 f1 | cat", "?join --c f1 f1", "?join --n f1 f1", "?join --z f1 f1 | od -c", "?join --zero f1 f1 | od -c", "?join --check f1 f1", "?join --nocheck f1 f1", "?join --ignore-c f1 f1", "?join --i f1 f1",
     "?join -zz f1 f1 | od -c", "?join -iz f1 f1 | od -c", "?join - | cat", "?join -- | cat", "?join -a1x f1 f1 | cat", "?join -1 | cat", "?join -t | cat", "?join -e | cat", "?join -o | cat", "?join -v | cat", "?join -j | cat", "?join -2 | cat")
case("echo -e 'a 1' > f1", "?join -1 0 f1 f1", "?join -1 x f1 f1", "?join -2 -1 f1 f1", "?join -2 '' f1 f1", "?join -j 0 f1 f1", "?join -j x f1 f1", "?join -j '' f1 f1", "?join -a 0 f1 f1", "?join -a 3 f1 f1", "?join -a x f1 f1", "?join -a '' f1 f1", "?join -a 1x f1 f1",
     "?join -v 0 f1 f1", "?join -v 3 f1 f1", "?join -v '' f1 f1", "?join -1 1 -1 2 f1 f1", "?join -2 3 -2 4 f1 f1", "?join -j 1 -j 2 f1 f1", "?join -1 2 -j 1 f1 f1", "?join -j 2 -2 3 f1 f1", "?join -1 2 -2 2 -j 3 f1 f1", "?join -e x -e y f1 f1", "join -e x -e x f1 f1",
     "?join -e '' -e x f1 f1", "?join -e x -e '' f1 f1", "join -e '' -e '' f1 f1", "?join -1 99999999999999999999 -2 1 f1 f1", "?join -1 9223372036854775807 -j 1 f1 f1", "?join -j 99999999999999999999 f1 f1", "?join -j 99999999999999999999 -1 99999999999999999998 f1 f1",
     "join -j 99999999999999999999 f1 f1", "join -a1 -a2 -j 99999999999999999999 -e - f1 f1", "?join -1 -5 f1 f1", "join -1 +1 -2 ' 1' f1 f1", "?join -1 '1 ' f1 f1", "join -1 01 f1 f1", "?join -1 1.0 f1 f1")
case("echo -e 'a 1' > f1", "?join -o 3.1 f1 f1", "?join -o 1.0 f1 f1", "?join -o 0.1 f1 f1", "?join -o a f1 f1", "?join -o '' f1 f1", "?join -o 1 f1 f1", "?join -o 1. f1 f1", "?join -o 2 f1 f1", "?join -o 0. f1 f1", "?join -o 1.1, f1 f1", "?join -o ,1.1 f1 f1", "?join -o 1.1,,2.1 f1 f1",
     "?join -o '1.1 ' f1 f1", "?join -o ' 1.1' f1 f1", "?join -o '1.1  2.1' f1 f1", "?join -o 1.x f1 f1", "?join -o 1.1x f1 f1", "?join -o 1.-1 f1 f1", "?join -o 1.+1 f1 f1", "join -o 1.01 f1 f1", "?join -o 12.1 f1 f1", "?join -o 00 f1 f1", "?join -o auto,0 f1 f1", "?join -o Auto f1 f1",
     "?join -o 1.99999999999999999999 f1 f1", "join -o 1.99999999999999999999 -e - f1 f1", "?join -o 0,1.1,2.99999999999999999999 f1 f1", "?join -o 1.1 f1 f1 -o a", "?join -o 1.1 -o 3 f1 f1", "?join f1 f1 -o 1.1 -o 3", "?join -o 1.1 3.1 f1 f1", "?join -o 1.1 f1 3.1 f1")
case("echo -e 'a 1' > f1", "?join nosuch f1", "?join f1 nosuch", "?join nosuch nosuch2", "?join nosuch -", "?join - nosuch", "?join -a 1 nosuch f1", "?join '' f1", "?join f1 ''", "mkdir d", "?join d f1", "?join f1 d", "?join d d", "?join d nosuch", "?join nosuch d", "?join - d < f1", "?join d - < f1", "?join d/ f1")
case("echo -e 'a 1\\nb 2' > f1", "mkdir d", "?join -a1 d f1", "echo -n > e", "?join e d", "?join d e", "?join -a2 e d", "?join --header d f1")
# options after the files, bundled and attached values
case(JA, JB, "join f1 f2 -a 1", "join f1 f2 -a1", "join f1 -a1 f2", "join -a1 -a2 -ie X f1 f2", "join -a1 -e- f1 f2", "join -e- -a1 -a2 -o 0,1.2,2.2 f1 f2", "join -a1 -v2 f1 f2", "join -a 1 -a 1 f1 f2", "join -v1 -v1 f1 f2", "join -o0 f1 f2", "join -o 0,1.2 f1 f2", "join -t: f1 f2",
     "?join -t'' f1 f2", "join -izt: f1 f2 | od -c | head -n 2", "join -1 1 -2 1 f1 f2", "join -11 -21 f1 f2", "join -j1 -- f1 f2", "join -j1 f1 f2", "join -ea f1 f2", "?join -v1v2 f1 f2")
# -z
case("echo -n -e 'a 1\\0b 2\\0c 3\\0' > z1", "echo -n -e 'a x\\0c y\\0d z\\0' > z2", "join -z z1 z2 | tr '\\0' '\\n'", "join -z -a1 -a2 z1 z2 | tr '\\0' '\\n'", "join -z -v1 z1 z2 | tr '\\0' '\\n'", "join -z -o 0,2.2 z1 z2 | tr '\\0' '\\n'", "join --zero-terminated z1 z2 | tr '\\0' '\\n'",
     "join -z z1 z2 | od -c | head -n 3", "join z1 z2 | od -c | head -n 3", "echo -n -e 'a 1\\0b 2' > z3", "join -z z3 z2 | od -c", "join -z -a1 z3 z2 | od -c")
case("echo -n -e 'a b\\nc\\0d e\\nf\\0' > z1", "echo -n -e 'a b\\nx\\0d e\\ny\\0' > z2", "join -z z1 z2 | od -c", "join -z -t ' ' z1 z2 | od -c", "join -z -t '' z1 z2 | od -c", "join -z -j 2 z1 z2 | od -c", "join -z -j 3 -a1 -e - z1 z2 | od -c", "join -z -o auto z1 z2 | od -c",
     "echo -n -e 'a\\nb\\0a\\nc\\0' > z3", "join -z -j 2 z3 z3 | od -c", "join -z -o 1.1,1.2,1.3,2.2 -e - z3 z3 | od -c")
case("echo -n -e 'b 1\\0a 2\\0c 3\\0' > z1", "echo -n -e 'a x\\0c y\\0' > z2", "?join -z z1 z2 | od -c", "?join -z --check-order z1 z2 | od -c", "?join -z -a1 z1 z2 | od -c", "echo -n -e 'a\\nb 1\\0a 2\\0c 3\\0' > z3", "?join -z --check-order z3 z2 | od -c", "?join -z -a1 z3 z2 | od -c",
     "echo -n -e 'a 1\\nb\\0a 2\\0' > z4", "?join -z -j 2 --check-order z4 z4 | od -c", "echo -e 'b 1\\0a 2\\n' | od -c | head -n 1")
# bigger files: the buffers
case("seq -w 1 400 | sed 's/$/ one/' > l1", "seq -w 3 3 400 | sed 's/$/ two/' > l2", "join l1 l2 | wc -l", "join -a1 l1 l2 | wc -l", "join -a2 l1 l2 | wc -l", "join -a1 -a2 l1 l2 | wc -l", "join -v1 l1 l2 | wc -l", "join -v2 l1 l2 | wc -l", "join l1 l2 | tail -n 2", "join -a1 l1 l2 | head -n 4", "join -v1 l1 l2 | tail -n 3",
     "join l2 l1 | tail -n 2", "join -a1 -a2 l1 l2 | sed -n '399,401p'", "cat l1 | join - l2 | wc -l")
case("seq 1 300 | sed 's/$/ x/' | sort > l1", "seq 100 2 500 | sed 's/$/ y/' | sort > l2", "join l1 l2 | wc -l", "join l1 l2 | head -n 3", "join -a1 -a2 l1 l2 | wc -l", "join -v1 l1 l2 | wc -l", "join -v2 l1 l2 | tail -n 2", "join -1 2 -2 2 l1 l2 | wc -l")
case("seq 1 40 | tr '\\n' ' ' > w1", "echo >> w1", "seq 20 60 | tr '\\n' ' ' > w2", "echo >> w2", "join -j 5 w1 w2", "join -j 20 -o 0,1.40,2.1 w1 w2", "join -j 40 w1 w2", "join -j 40 -o 0,1.1,2.1,2.41 -e - w1 w2", "join -a1 -a2 -j 3 -o 0,1.40,2.40 -e - w1 w2", "join -o auto -a1 -a2 w1 w2 | wc -w", "join -a1 w1 w2 | wc -w")
case("echo 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa x' > f1",
     "cat f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 f1 > f2", "join f1 f2 | wc -l", "join f1 f2 | wc -c", "join -o 1.2,2.2 f2 f2 | wc -l", "join f2 f1 | wc -c")


# -o 0 on a line with no partner is the join field of the file it came from
case("echo -e 'x a\\ny b\\nz d' > f1", "echo -e 'a 1\\nc 2\\nd 3' > f2", "join -1 2 -2 1 -a2 -o 0,2.2,1.1 -e - f1 f2", "join -1 2 -2 1 -a1 -a2 -o 0,2.2,1.1 -e - f1 f2",
     "join -1 2 -2 1 -v2 -o 0,2.2 f1 f2", "join -1 2 -2 1 -v1 -o 0,1.1 f1 f2", "join -1 2 -2 1 -a2 f1 f2")
# the order warning prints the line only up to a NUL, as GNU does
case("echo -n -e 'b 1\\na\\0y 2\\nc 3\\n' > n1", "echo -e 'a 1\\nc 3' > n2", "?join n1 n2", "?join --check-order n1 n2")

# the tail of a file that has no partner left, with different join fields; the shapes -o can take
case("echo -e 'x a\\ny b\\nz d\\nw e' > f1", "echo -e 'a 1' > f2", "join -1 2 -2 1 -a1 f1 f2", "join -1 2 -2 1 -v1 f1 f2", "join -1 2 -2 1 -a1 -o 0,1.1 -e - f1 f2",
     "echo -e 'a x\\nb y' > g1", "echo -e 'a 1 2\\nb 3 4' > g2", "join -o 0 1.2 2.2 g1 g2", "?join -o 1.1 g1 g2 2.1 | cat", "?join -o 1.1 g1 2.1 g2 | cat", "join -o 0 -o 2.2 g1 g2", "join -o 1.2 -j 1 g1 g2",
     "?join -o 1.1 -j 1 2.1 g1 g2 | cat")
# names that are quoted in messages: a leading ~ or #, an apostrophe, and --=
case("echo a > f1", "?join '~x' f1", "?join '#x' f1", "?join 'x~' f1", "?join -t \"a'b\" f1 f1", "?join f1 f1 '~a' | cat", "?join f1 f1 'a#b' | cat", "?join --=x f1 f1 | cat", "?join -t 'a\\b' f1 f1")

def run_all():
    out, bad = [], []
    for cmds in CASES:
        d = tempfile.mkdtemp(prefix="gen-text-")
        parts = []
        for c in cmds:
            may_fail = c.startswith("?")
            # stderr is merged in order: RingTail's console shows both, and a
            # tool flushes what it wrote before it complains
            # du: GNU's numbers are for what a file holds, not for clusters; the
            # cases use files whose sizes come out the same (see the du section)
            pre = 'du() { command du --apparent-size "$@"; }; ' if re.search(r"(^|[ |;])du ", c) else ""
            # printf and sha256: bash's own printf is a builtin, and GNU's sha256sum is
            # not named sha256; exec -a gives the messages the name RingTail's tools use
            if re.search(r"(^|[ |;(])printf( |$)", c.lstrip("?")):
                pre += 'printf() ( exec -a printf /usr/bin/printf "$@" ); '
            if re.search(r"(^|[ |;(])sha256( |$)", c.lstrip("?")):
                pre += 'sha256() ( exec -a sha256 /usr/bin/sha256sum "$@" ); '
            p = subprocess.run(["bash", "-c", pre + c.lstrip("?")], cwd=d, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10, env=dict(os.environ, LC_ALL="C"))
            if p.returncode and not may_fail:
                bad.append(f"{c!r}: {p.stdout.decode('latin-1').strip()!r}")
                break
            # a program that fails makes RingTail's shell print its status
            # older diffutils say "char" where newer ones say "byte"
            parts.append(p.stdout.decode("latin-1").replace(" differ: char ", " differ: byte ").replace(": No such file or directory", ": no such file or directory").replace(": Is a directory", ": is a directory").replace("\u00e2\u0080\u0098", "'").replace("\u00e2\u0080\u0099", "'") + (f"[exit status {p.returncode}]" if p.returncode else ""))
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
        f.write("# sed scripts and the exit status GNU sed gives them (0: not an error)\n")
        f.write("ERRS = [\n")
        for sc in ERRS:
            st = subprocess.run(["bash", "-c", "echo a | sed -n \"$0\" >/dev/null 2>&1", sc], capture_output=True).returncode
            f.write(f"    ({sc!r}, {st}),\n")
        f.write("]\n")
    print(f"{len(cases)} cases")

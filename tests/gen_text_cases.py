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
        f.write("# sed scripts and the exit status GNU sed gives them (0: not an error)\n")
        f.write("ERRS = [\n")
        for sc in ERRS:
            st = subprocess.run(["bash", "-c", "echo a | sed -n \"$0\" >/dev/null 2>&1", sc], capture_output=True).returncode
            f.write(f"    ({sc!r}, {st}),\n")
        f.write("]\n")
    print(f"{len(cases)} cases")

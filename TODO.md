# To do

Work for the next sessions, as of v0.28.0 and the programs added after it
(up to `cal`). Each task stands on its own: do them in any order, one
commit (or a few) each. The first sections are meant for Sonnet on high;
the last one is work to keep for Opus on high, because the hard part there
is the design, not the typing.

Checked against the code when this was written. Do not add things that
already exist: `fetch` already reads chunked bodies (fetch.c, the
`chunked` path), so that is not on this list.

## House rules (read first)

Every task ends the same way, or it is not done:

- `make test` passes: the host tests, then the whole QEMU suite. Run the
  full suite once at the end, not only the new tests; earlier tests leave
  state behind (TCP connections stay in TIME-WAIT for 30 s, see
  `TCP_TIME_WAIT_MS`), and the memory checks notice.
- A new or changed program is tested in QEMU the way its neighbours are.
  Text tools: add cases to `tests/gen_text_cases.py` and regenerate
  `tests/text_cases.py` with the real GNU tools, so the expected output is
  GNU's, not ours. Network: `tests/net_peer.py` (a raw-frame peer that plays
  DHCP, DNS and NTP), `tests/dns_server.py`, `tests/ntp_server.py`.
- Parsers get host tests too (under ASan/UBSan, like `tests/test_netmsg.c`),
  and the tests are mutation-checked: break the code on purpose in a few
  places and make sure a test fails each time. Add tests until one does.
- Update the README: the row in the programs table, the shell paragraph for
  built-ins, and the Limits section when a limit goes away or a new one
  appears. Keep its voice: plain sentences, no marketing.
- When a program is done, the report opens with one plain sentence on what
  it lets you do (what it is for, not its options), before the details.
- No PROGRESS.md entry or version bump unless a release is asked for.
- Commit on the session branch. Never push to main, and never force-push,
  without being asked.

## Things that bit before

- The disk takes DOS 8.3 names only (`tools/mkdisk.py` refuses the rest),
  so a program's name is at most 8 characters.
- The shell has no `2>&1` yet (an item below). Error cases in
  `gen_text_cases.py` end in `| cat`; see the `du` and `cal` cases.
- `echo -e` has no `\f` or `\v` yet (an item below): use `\x0c` and `\x0b`.
- A spawned program gets its own copy of each file offset, not a shared
  one (see "find -exec" below). Anything that runs programs into a file
  it is also writing has to allow for that.
- Not every reference is GNU: `cal` is checked against BSD `ncal`, because
  that is the layout people know. Check the reference's `--version`, say
  which one it is in the commit, and make the generator refuse to run
  against the wrong one (the `cal` section does).
- `rtcset` in `tests/programs` sets the clock for a test that needs a
  given date. Put the clock back afterwards (the `cal` block shows how).
- `make test` takes about ten minutes. Run it in the background and wait
  on its log.

## Small programs

Good warm-ups. Each is a new file in the folder its job puts it in (the
README says which, under "Programs on the disk"), found by the Makefile on
its own, with GNU-checked cases. The first sentence of each says what it
is for.

**printf** (`util/`). Prints exactly what you ask for, with no newline
unless you put one, and lines up columns. `printf FORMAT [ARG...]`: `%s
%b %c %d %i %u %o %x %X %%`, the flags `- + space # 0`, width and
precision (`*` too), the escapes `echo -e` takes, and the format used
again while arguments are left, as GNU does. Numbers are 64-bit, as
GNU's; an argument that is not a number gives GNU's message and status 1,
and the printing goes on. `'c` as a number is the character's code. For
`%f %e %g`, check whether the library printf has floating point; if not,
refuse them with a message and say so in the Limits, rather than write a
float formatter here. It matters more once the shell has scripts.

**split** (`text/`). Cuts a big file into numbered pieces that `cat`
puts back together. `split [-l N | -b SIZE] [-a LEN] [-d]
[--additional-suffix=S] [FILE [PREFIX]]`: pieces `xaa`, `xab` and on
(`x00` with `-d`), 1000 lines each by default; `-b` takes GNU's `K M`
suffixes. Running out of suffixes is GNU's error. In the cases, check
the pieces with `ls` and `cat x*`.

**join** (`text/`). Puts together the lines of two sorted files that
share a key, like looking things up in a table. `join [-i] [-t C] [-1 F]
[-2 F] [-j F] [-a 1|2] [-v 1|2] [-e STR] [-o LIST|auto] [--check-order |
--nocheck-order] [--header] FILE1 FILE2`. Fields are split at runs of
blanks unless `-t`. The order warnings follow the same GNU rules as in
`comm`: share that code if it can be shared.

**factor** (`util/`). Breaks numbers into their primes. Small, and fun
to watch. `factor [NUMBER...]`, or numbers from standard input, up to
2^64-1, printed as GNU does (`12: 2 2 3`). Trial division is far too slow
near 2^64: use Miller-Rabin (the deterministic bases for 64 bits) and
Pollard's rho. gcc for i386 has no `__int128`, so the 64-by-64 multiply
modulo n is done by hand; host-test it against Python on random and edge
values (primes near 2^64, squares of big primes, 2^64-1). A bigger
number is a clear error and a Limits line.

## Features in programs that exist

**find -exec into a file.** A spawned program gets its own copy of each
file descriptor with its own offset (`src/fs/file.c` says so), so when
`xargs` ran with `> out`, each run wrote over the one before from the
start of the file; `xargs` now seeks to the end after each run. `find
-exec` spawns the same way with no such seek, so `find . -name '*.txt'
-exec cat {} ; > out` very likely keeps only the last file. Write that
test first and watch it fail, then fix it as `xargs` did: flush before
each run, seek to the end after it, and check `-print` mixed with
`-exec`. If it does not fail, find out why before changing anything. The
real fix is in the Opus section.

**echo -e with \f and \v.** GNU's `echo -e` turns `\f` into a form feed
and `\v` into a vertical tab; ours prints them as they are. Add both,
with cases. (The `cal` cases use `\x0c` and `\x0b`; leave them.)

**sort with several keys.** `sort` takes one `-k`. GNU takes any number,
tried in order, each with its own letters (`-k2,2n -k1,1r`); a key with
no letters takes the global ones. The whole-line tie-break stays last.
GNU cases for mixed keys with `-t`, `-s`, and `-u` (which compares only
the keys).

**Standard error redirection in the shell.** `2> FILE`, `2>> FILE` and
`2>&1`, including `a 2>&1 | b`, which sends both into the pipe.
`spawn_opts_t` already carries all three descriptors, so this is the
shell's parser and the place it fills them in (`saklas.c`).
Redirections apply left to right, as in sh: `> f 2>&1` sends both to
`f`, and `2>&1 > f` sends errors to the screen. Test both orders. Update
the README shell paragraph. Leave the old `| cat` cases as they are.

**tail -f.** Keep reading after the end of the file, checking its size
every so often (sleep between checks, do not spin), until Ctrl+C. If the
file gets shorter, say "file truncated" and start from its end, as GNU
does. Only for files; with a pipe, `-f` does nothing extra. The QEMU test
can append to the file from a second step and check the new lines show
up.

**Search in more.** `/pattern` and `n` for the next match, using the regex
code `grep` uses (the same syntax), with the match line put at the top of
the screen and "Pattern not found" otherwise. More in `more.c` than it
sounds: keep the screen redraw honest when the match is on the current
page.

**date with a format and date -s.** `date` is a shell built-in in
`saklas.c`. Add `date +FORMAT` with the common `strftime` codes (`%Y %m %d
%H %M %S %j %a %b %s %F %T %%`, and an unknown code printed as it is, as
GNU does), and `date -s "YYYY-MM-DD HH:MM:SS"` setting the clock through
`SYS_SETTIME` (syscall 61), which already refuses times before the build.
`%s` and the day of the week come from `src/lib/civil.c`.

**touch -d, and a call to set a file's date.** `touch` cannot set a date,
because there is no system call for it (the README says so). Add one
(the next free number after `SYS_SETTIME`), writing the FAT date and time of
the directory entry, then `touch -d "YYYY-MM-DD HH:MM:SS"` and `touch -r
FILE`. FAT keeps time to two seconds: say that in the README rather than
pretend. This touches the file system code, so add a host test in
`tests/test_heap_diskfs.c` for the entry it writes. Once the call exists,
`cp -p` (keep the date) is a few lines: add it, with cases.

## Network

**Sync the clock when the network comes up late.** At boot, init runs
`sntp -b` only if DHCP gave an address in time. `net dhcp` (the shell
built-in, `net_dhcp()` in `saklas.c`) already waits up to 10 s for the
lease; when it gets one, have it run `sntp -b` in the foreground the way the
shell runs any program, so going back to DHCP sets the clock. A first lease that comes
long after boot with nobody asking can stay a limit; reword that sentence
in the README Limits to match. Test with `net_peer.py`: `net static ...`,
then `net dhcp`, and expect the Clock line "by 10.0.2.2".

**DNS over TCP when the answer is truncated.** Today a truncated UDP
answer is `DNS_E_TRUNCATED` ("no retry over TCP"). Retry the same question
over TCP to the same server (RFC 7766: each message prefixed by its length in
two bytes), with the same checks on the answer as the UDP path. The library
is `src/programs/lib/dns.c` and `dnsmsg.c`. Extend `tests/dns_server.py` to
set TC on a big answer and serve TCP, and test both a success and a server
that refuses TCP. Remove the sentence from the Limits and the "no retry
over TCP" wording from the error and its comment in `dnsmsg.h`.

**ARP probe before taking a DHCP address (RFC 5227).** Before using an
offered address, send ARP probes for it (sender address 0.0.0.0); if
anyone answers, send DHCPDECLINE and start over. Keep the waits short
(the RFC's times add seconds to every boot; the boot test will tell you
if it is too slow, so use the low end and say so). In `src/net/dhcp.c`;
the DHCP message code is `dhcpmsg.c`, which has host tests to extend for
DECLINE. In `net_peer.py`, add a mode that answers the probe for the
first address and check the client declines it and takes the second.

**ICMP port unreachable for UDP sockets.** Nothing reads it now, so a dead
DNS server is found by timeout. When an ICMP port unreachable comes back
quoting a datagram from one of our UDP sockets, match it to the socket
(quoted source port, destination address and port) and make its next
receive fail with `ECONNREFUSED`; then have the resolver move to the next
server at once on that error. Kernel code in `src/net/`: careful with the
quoted header (check its lengths before reading it; host-test that parse).
Test with `net_peer.py` sending the unreachable for a DNS query, and check
`host` gives up in well under the 4 s timeout.

**Updating the root certificates.** The roots are `/ROOTS.PEM`, copied in
when the disk is built. Find out whether `fetch -o /ROOTS.PEM
https://curl.se/ca/cacert.pem` (or from a local `httpd`) already works;
it is a few hundred KB and has text between the certificates, so there may
be a size or parse limit in the way. Fix what stops it, make sure a
broken or empty download cannot replace the good file (write to a temporary
name, check it parses and has at least one root, then rename), and write
the README sentence that replaces "nothing updates them".

**fetch asks for gzip.** Send `Accept-Encoding: gzip` and decode the body.
This means writing inflate (RFC 1951) and the gzip wrapper (RFC 1952, check
the CRC-32 and size at the end). Big but self-contained: host tests against
Python's `zlib` and `gzip` with many generated inputs (stored, fixed and
dynamic blocks, long distances, empty), plus malformed ones that must fail
cleanly and never read or write out of bounds. Then the QEMU test with
`httpd` or a Python server sending a gzip body, chunked and not. Remove
"does not ask for compressed pages" from the Limits.

## Keep for Opus on high

These need a design decision first; Sonnet would build the wrong thing
confidently.

**Shared file offsets.** A copy of a file descriptor keeps its own
offset (`src/fs/file.c`), where in POSIX a parent and the programs it
starts share one open file and one offset. That is why `xargs` seeks
after each run and `find -exec` needs to. The fix is a shared,
reference-counted open file that `spawn` (and `dup2`: check what it does
now) hands on, after which those seeks can go. It changes when a file is
really closed and who frees it, across tasks: a mistake there is a
use-after-free that shows up a week later.

**Back-references in grep and sed.** `\1` inside a pattern (the README
says "No back-references"). An automaton cannot match them; GNU switches
to a backtracking matcher only for patterns that have one. The trap is
that POSIX wants the leftmost-longest match, which a plain backtracker
(leftmost-first, as in Perl) does not give, and `sed s///` shows the
difference in what it replaces. How to keep POSIX semantics, and how to
bound the time, is the design.

**Shell scripting.** The shell has no `;`, `&&`, `||`, `$?`, variables or
scripts, which is why `test`/`[` is not on the list above: without them it
would do nothing. What the language looks like, how much of POSIX to take,
and how a script file runs is a design question.

**A resolver cache.** The resolver is a library linked into each program,
so a cache there only lives as long as one program. A cache worth having is
shared, which means the kernel or a daemon, and TTLs against a clock that
`sntp` can move.

**Clock discipline and better time.** Several servers and a choice between
them, slewing instead of stepping, NTS. Each is a trust and design call.

**A graphics mode past 13h** (Bochs VBE framebuffer), **signals**,
**fork/exec**, **SMP**, **IPv6**, and **TLS session resumption**: each one
reaches into the scheduler, memory, the syscall interface or the protocol
state machine, where a quiet mistake costs days.

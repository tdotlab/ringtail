# To do

Work for the next sessions, as of v0.28.0. Each task stands on its own: do
them in any order, one commit (or a few) each. The first sections are meant
for Sonnet on high; the last one is work to keep for Opus on high, because
the hard part there is the design, not the typing.

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
- No PROGRESS.md entry or version bump unless a release is asked for.
- Commit on the session branch. Never push to main, and never force-push,
  without being asked.

## Small programs

Good warm-ups. Each is a new file in `src/programs/`, added to the Makefile
the way the others are, with GNU-checked cases.

**du.** `du [-s] [-h] [-a] [PATH...]`, walking directories the way `find`
does. Decide whether sizes are in clusters (what the disk really uses, as
`df` counts) or bytes, and say which in the README. GNU's numbers will not
match a FAT disk, so test it against known files on the test disk instead.

**cal.** `cal [[MONTH] YEAR]` in GNU's layout, using the calendar code in
`src/lib` (`civil.c`, which turns dates into Unix time and back; the day of the week falls out of that); today is
highlighted only on a terminal. Check the 1752 switch: GNU does Julian
before September 1752, so either do the same or refuse years before 1753
and say so.

## Features in programs that exist

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
`tests/test_heap_diskfs.c` for the entry it writes.

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

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
- A spawned program gets its own copy of each file offset, not a shared
  one (see "Shared file offsets" in the Opus section). Anything that runs
  programs into a file it is also writing has to allow for that, as `xargs`
  and `find -exec` do by seeking to the end after each run.
- Not every reference is GNU: `cal` is checked against BSD `ncal`, because
  that is the layout people know. Check the reference's `--version`, say
  which one it is in the commit, and make the generator refuse to run
  against the wrong one (the `cal` section does).
- `rtcset` in `tests/programs` sets the clock for a test that needs a
  given date. Put the clock back afterwards (the `cal` block shows how).
- Hardware waits are timed by the clock, not by counting polls. The ATA
  driver once gave the drive 2 million polls, about 0.1 s under QEMU,
  and a slow fsync on the host turned into a failed write in one full
  test run. The disk is read before interrupts are on, so
  such waits use `stopwatch_ms`, which keeps time without the timer
  interrupt. The disk tests in the QEMU suite slow the disk and inject
  errors to keep this honest.
- QEMU's IDE reset cancels an in-flight PIO request synchronously, so
  a test that suspends a request (blkdebug `break`) and lets the driver
  time out and reset freezes QEMU itself. Make a drive look hung with a
  throttle slower than the timeout instead.
- The kernel is linked at -2 GB and reaches RAM through the direct map,
  so a physical address is never a pointer: go through `phys_to_virt`
  (nothing is mapped below 1 GB, so forgetting faults at once). Frames
  come from the top of RAM, above 4 GB on a big machine, so a device
  that can only reach 32-bit addresses needs frames from a low pool,
  which does not exist yet.
- QEMU's emulation (TCG) clears the direction flag on SYSCALL whatever
  FMASK says, so no test under it notices if FMASK stops clearing DF;
  `sysregs` catches it only on the `int 0x80` side (isr_common's `cld`).
  And a register the kernel happens not to touch on a call's path comes
  back right even if the exit stub forgets to restore it: getcwd leaves
  r8 and r10 alone, so `sysregs` catches a missing `pop rsi` but not a
  missing `pop r10`.
- QEMU cannot tell some interrupt bugs apart. Its I/O APIC ignores the
  polarity bit, the e1000 can never lose an edge (reading ICR clears
  every cause and drops the line), and SeaBIOS hands over ACPI 1.0
  tables, so routing IRQ 11 active low or edge triggered passes the
  suite, and so would a broken XSDT path. The MADT's GSI override for
  IRQ 0 is never used either: the tick comes from the local APIC timer,
  and the PIT's input stays masked. Check those by reading the code
  against the MADT and the 82093AA datasheet, not by a green run.
- `timer_micros` adds a tick when the tick's interrupt is pending but
  not taken (the counter has already gone round). That only happens
  with interrupts off across a tick, which no test arranges, so the
  suite passes with the check removed, on the PIC and the APIC alike.
- A QEMU test's work directory must keep its socket paths under 108
  bytes (`serial.sock` inside it): a deep scratch directory makes QEMU
  refuse to start. Pass a relative path.
- `make` does not rebuild objects when compiler flags change. After
  changing `KCFLAGS` (adding `-mcmodel=kernel`, say), delete
  `build/x86_64/k` first, or the link fails with relocations "truncated
  to fit" against objects built the old way.
- `make test` takes about ten minutes. Run it in the background and wait
  on its log.
- The machine is x86_64 now. The kernel is built with `-mno-red-zone`
  (interrupts land on the stack it is running on) and
  `-mgeneral-regs-only` (it never touches the SSE registers, which belong
  to whichever program was running). The kernel links with
  `-z max-page-size=0x1000`, or the Multiboot header ends up past the
  first 8 KB of the file, where GRUB stops looking. Code that only one
  machine needs goes in `src/arch/x86_64`, not behind an `#ifdef`
  elsewhere.
- System call handlers return `long`, so `-ENOENT` comes back
  sign-extended in rax. A wrapper that returns a pointer or an address
  checks it with `IS_ERR` (`ringtail/abi.h`), not `< 0` on a `uint32_t`.
- Host tests build the kernel's own code with the host gcc, so they
  only mean something on a 64-bit host, where the pointer sizes match.

## Small programs

Good warm-ups. Each is a new file in the folder its job puts it in (the
README says which, under "Programs on the disk"), found by the Makefile on
its own, with GNU-checked cases. The first sentence of each says what it
is for.

## Features in programs that exist

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

## After the move to 64-bit

RingTail went from i386 to x86_64 after v0.28.0, and i386 was dropped
once the whole suite passed on x86_64 (to bring it back, revert the commit
"drop i386: x86_64 is the only target"; the port before it built both).
Everything below was left out of that move on purpose, to keep it to one
change at a time. The kernel has since moved to the top half, with a
direct map of all of RAM (so RAM above 1 GB, and above 4 GB, is used),
but programs still get the 32-bit range, 1 GB to 3 GB.

**Programs in the whole lower half.** The kernel no longer uses any of
the lower half, so programs could have up to 128 TB instead of 2 GB: a
heap that grows past 3 GB, a bigger stack, room for mmap later. It is
`USER_BASE`/`USER_TOP` in mm.h and `src/programs/user.ld`, plus paging.c,
whose address-space code walks one PDPT (`user_pdpt`, PML4 entry 0, so
512 GB) and the user range inside it. Keep the first page, at least,
unmapped. Every program is rebuilt anyway, so the link address can move.

**factor on unsigned __int128.** `factorcore.c` builds each 128-bit
product out of four 32-bit multiplications, because i386 had no 128-bit
type. On x86_64 gcc has `unsigned __int128`, and one `mulq` gives the
product. Montgomery's reduction can stay (it still saves the division);
only the product changes. Time it on the suite's big semiprimes, and the
GNU cases say whether the answers stay right.

**UEFI.** GRUB boots RingTail through Multiboot 1 from a BIOS. Most real
machines boot UEFI. Multiboot2 through GRUB's EFI build is the small step;
a loader of our own (or Limine) is the big one. The framebuffer from UEFI
would also replace VGA text, which UEFI machines may not have.

**SMP.** The boot CPU takes its interrupts from the local and I/O APICs
the ACPI MADT describes (`src/drivers/acpi.c`, `apic.c`), and its tick
from the local APIC timer; the MADT's list of processors is counted and
the banner says how many there are. The others are never started. That
needs the INIT-SIPI-SIPI sequence and a real-mode trampoline below 1 MB
to bring each one up to long mode; then per-CPU data, locks that hold
across CPUs, and a scheduler that knows there is more than one of them.
It is listed again under Opus below for that reason. What is per-CPU
already, and so needs one per CPU then: the TSS (rsp0 and the three IST
stacks) and `cpu_t` in gdt.c, which the SYSCALL entry finds through
swapgs and KERNEL_GS_BASE; and the local APIC with its timer, set up
for the boot CPU only (`apic_init`, `timer_init`'s calibration). The
kernel keeps gs at 0 outside that swapgs pair, so nothing else uses it
yet; a `this_cpu` for the rest of the kernel means swapgs on every entry
from ring 3, and the NMI path then has to work out which gs it got
(Linux's "paranoid" entry).

**PCI interrupts on real boards.** A PCI device's interrupt is taken
from its interrupt line register, an ISA IRQ, and routed to the I/O APIC
input of that number, with the MADT's override or else the ELCR saying
how it triggers. That holds on QEMU's pc and q35, which wire PCI
interrupts to the ISA inputs too. Many real boards wire them only to
inputs 16 and up, which only the DSDT's `_PRT` says, in AML. MSI, where
the card has it, needs no routing at all and is the cheaper way out for
the network card.

**ACPI power-off.** `machine_poweroff` writes QEMU's and the other
emulators' fixed ports. The FADT has the real PM1a control port; the
sleep type for S5 is in the DSDT's `\_S5_` package, which can be found
by searching the AML bytes without an interpreter, as many hobby
kernels do.

**AVX.** Programs get x87 and SSE, saved with `fxsave`. AVX needs `xsave`
and a bigger save area, sized from CPUID.

**ARM64.** The `src/arch` split is where a port starts, but it is far from
all of it: the drivers talk to PC hardware (port I/O, PCI through 0xCF8,
VGA, PS/2, ATA PIO, the CMOS clock), and an ARM board has none of that.
It would need a device tree, a UART such as the PL011, and virtio devices
under QEMU's `virt` machine, before the shell could start.

## Keep for Opus on high

These need a design decision first; Sonnet would build the wrong thing
confidently.

**Shared file offsets.** A copy of a file descriptor keeps its own
offset (`src/fs/file.c`), where in POSIX a parent and the programs it
starts share one open file and one offset. That is why `xargs` and
`find -exec` seek to the end after each run. The fix is a shared,
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

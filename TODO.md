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
- Older error cases in `gen_text_cases.py` end in `| cat`, from before
  the shell had `2>&1`; see the `du` and `cal` cases. They still work.
- `echo`, `printf`, `true`, `false`, `test` and `pwd` are shell
  built-ins, and a built-in wins over the program of the same name. A
  QEMU test of the program says `run printf ...`.
- Commands in `tests/qemu_test.py` are real shell now: quote `;`, `(`,
  `)`, `$`, `*` and `|` meant for a program (`find . -exec cat {} \;`,
  `find '(' ... ')'`), as you would at a Linux prompt.
- A spawned program shares its parent's open files, offsets included
  (`file.c`). A child given a file as standard input reads it on from
  where the parent is and leaves the parent where it stopped, so a
  program that runs others and still reads its own input (as `xargs`
  does) must hand them something else; `xargs` gives them an empty pipe.
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
  errors to keep this honest. A drive that hangs and comes back is
  tested on the host instead (`tests/test_ata.c`, a model drive on a
  model clock), since three 30 s timeouts would add two minutes to the
  suite; throttle the drive to 0 writes and lift it to try it by hand.
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
- On the PIT (no APIC, so no TSC timekeeping), `timer_micros` adds a
  tick when the tick's interrupt is pending but not taken (the counter
  has already gone round). That only happens with interrupts off
  across a tick, which no test arranges, so the suite passes with the
  check removed.
- A QEMU test's work directory must keep its socket paths under 108
  bytes (`serial.sock` inside it): a deep scratch directory makes QEMU
  refuse to start. Pass a relative path.
- A CPU that faults before it has loaded the IDT, in the trampoline or
  early in `ap_main`, triple-faults, and QEMU resets the whole machine:
  a startup bug shows as the banner printed over and over, not as a
  CPU missing from the count.
- `task_wakeup` ran before `task_init` (the console's mutex is let go
  of from the first kprintf) and walked a task list that was not there
  yet, reading the real-mode interrupt table at address 0, which is
  still mapped then. It got away with it for years because `next` sat
  at an offset where that memory ended the loop; shrinking `task_t`
  moved it and the boot died with no output at all. It now returns
  early with no tasks. A boot that prints nothing before the banner:
  `-d int` and `info registers` in the monitor show where.
- Two tasks writing one shared open file at once each take its mutex
  (`file.c`), so no two writes land on the same bytes. No test can see
  the mutex yet: under the big kernel lock a write that does not block
  on the disk cannot be interrupted by another, and the suite passes
  with it removed. It matters once the lock is split or the disk
  writes through.
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
a loader of our own (or Limine) is the big one. The console already draws
on a linear framebuffer (GRUB's, see `read_framebuffer` in kernel.c), so
UEFI's GOP framebuffer would slot in where it is; the VGA text and mode 13h
fallbacks are what UEFI machines may not have.

**SMP, past the big lock.** Tasks run on every CPU, under one kernel
lock (`kernel_lock`, `src/kernel/interrupt.c`): a CPU holds it whenever
it runs kernel code and lets go on the way to ring 3 and in its idle
halt. So two CPUs never run kernel code at once, and a task that spends
its time in syscalls gains nothing from the second CPU. What is left:
- Finer locks. Every critical section is still `irq_save` (63 of them),
  correct only because the big lock is held around it. Splitting it
  means a spinlock per subsystem (the scheduler's lists, the heap, the
  frame pool, the VFS, the network stack), each taken with interrupts
  off, and a lock order; the scheduler and the heap first, since every
  syscall touches them.
- Device interrupts all go to the boot CPU. Spreading them (the I/O
  APIC's destination field per input) needs nothing new, but buys
  nothing until the kernel itself runs in parallel.
- No CPU affinity, and no migration cost: the round robin hands the
  next ready task to whichever CPU asks, so a task hops between CPUs.
- The TSC is trusted to run at a constant rate and to read the same on
  every CPU, as on QEMU and any CPU with an invariant TSC (CPUID
  80000007h EDX bit 8, not checked). Without one, SMP should be refused
  or the tick count kept by one CPU.
- TLB shootdowns are still not needed: kernel mappings are only ever
  added, and an address space is freed only once its task is dead and
  off every CPU. The first `munmap` of a page another CPU may have
  cached, in a process with threads, changes that.

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

**The rest of signals.** Signals work (`src/kernel/signal.c`,
`src/arch/x86_64/sigframe.c`, `sigtest`), but leave out: SA_RESTART (an
interrupted call always returns EINTR); siginfo and a ucontext for the
handler (it gets only the number; a fault handler cannot see the address);
`sigsuspend`/`pause` and `alarm`; SIGPIPE from sockets (they return EPIPE
only); `kill` of a whole job (a negative id); and a fresh FPU state for a
handler (it starts with the interrupted code's MXCSR and x87 stack, where
Linux gives it the default).

A smaller one in the same place: a pipe holds 4 KB here and 64 KB on
Linux. A tool that writes more than 4 KB in `xargs tool | head -n 1` can
be killed by SIGPIPE here (and xargs says so) where on Linux the whole
output fits in the pipe before head leaves. Up to 4 KB matches, since
`lio_out` writes in glibc's 4 KB pieces.

**ARM64.** The `src/arch` split is where a port starts, but it is far from
all of it: the drivers talk to PC hardware (port I/O, PCI through 0xCF8,
VGA, PS/2, ATA PIO, the CMOS clock), and an ARM board has none of that.
It would need a device tree, a UART such as the PL011, and virtio devices
under QEMU's `virt` machine, before the shell could start.

## Keep for Opus on high

These need a design decision first; Sonnet would build the wrong thing
confidently.

**Back-references in grep and sed.** `\1` inside a pattern (the README
says "No back-references"). An automaton cannot match them; GNU switches
to a backtracking matcher only for patterns that have one. The trap is
that POSIX wants the leftmost-longest match, which a plain backtracker
(leftmost-first, as in Perl) does not give, and `sed s///` shows the
difference in what it replaces. How to keep POSIX semantics, and how to
bound the time, is the design.

**The rest of the shell.** The language (`shcore.c`) is POSIX shell
without `trap`, `getopts`, `command`/`type`, `readonly`, aliases and `~`.
`trap` wants signals delivered to the shell as events it runs at a safe
point (it has `sh_interrupted` for SIGINT only). Add each with cases in
`tests/gen_sh_cases.py`, regenerated against bash --posix, and a QEMU
case where the console matters. An environment for programs (`export`
reaching them) needs spawn to carry one, which is an ABI change.

**A resolver cache.** The resolver is a library linked into each program,
so a cache there only lives as long as one program. A cache worth having is
shared, which means the kernel or a daemon, and TTLs against a clock that
`sntp` can move.

**Job control.** SIGCHLD, SIGTSTP/SIGSTOP and SIGCONT, Ctrl+Z, and `fg`
and `bg` in the shell. A stopped task is a new scheduler state, a stop has
to reach the parent (SIGCHLD and a wait that sees it), and the terminal has
to change hands when a job stops as well as when it ends.

**Clock discipline and better time.** Several servers and a choice between
them, slewing instead of stepping, NTS. Each is a trust and design call.

**The desktop, stages 2 to 4.** Stage 1 is done: GRUB's 1024x768
framebuffer, the console drawn in it (`src/drivers/console.c`, the
`console` kernel task), native graphics at full size (`GFX_NATIVE`,
`SYS_GFX_PUT`, `src/drivers/gfx.c`) and kenoma rebuilt on it. What is left,
in order, each one usable on its own:

- A window server. One user program owns the screen (it already can, with
  `GFX_NATIVE`) and composites windows; clients draw into memory they
  share with it and send it damage rectangles over a pipe or a socket.
  Shared memory is the new kernel piece: a call that maps the same frames
  into two address spaces, freed when both let go. Input goes to the
  server, which routes keys to the focused window and the pointer to the
  one under it.
- Pseudo-terminals and a terminal window. A pty pair in the kernel (the
  tty's line discipline on one side, a plain pipe-like end on the other),
  `tty_size` answering per pty, SIGWINCH on resize, then a terminal
  program that runs saklas on the pty and draws the cells in a window
  with the console font. After that the shell, Octavo and gauge run in
  windows unchanged.
- A small toolkit (buttons, lists, text fields, menus) on the shared
  buffers, a taskbar, and kenoma's launcher as a window-server client.
  Then a file browser and a settings window.

Decide the window protocol before writing either side: who owns damage
tracking, whether clients may resize themselves, and what happens when
a client stops answering.

**fork/exec**, **finer kernel locks**, **IPv6**, and **TLS session resumption**: each one
reaches into the scheduler, memory, the syscall interface or the protocol
state machine, where a quiet mistake costs days.

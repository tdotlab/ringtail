# RingTail OS

A small 32-bit x86 operating system written from scratch in freestanding
C and NASM. GRUB boots it through Multiboot. It runs ELF programs from a
FAT16 disk in separate, protected address spaces, and it has a preemptive
scheduler, a shell, a text editor, and a mouse-driven desktop in VGA mode
13h. It is on the network too: an Intel e1000 driver, ARP, IPv4, ICMP,
UDP and TCP, an address from DHCP, and a DNS resolver, so `ping
example.com` works and `fetch` gets a page over HTTP.

Current version: **v0.24.4 "Handshake"**. See PROGRESS.md for the history.

## Building and running

You need nasm, grub-mkrescue (with xorriso), qemu-system-i386, python3
and a C compiler that can target i386. An `i686-elf-gcc` cross compiler
is used when one is installed. Otherwise the host gcc works with
`-m32`; on Debian and Ubuntu that means gcc-multilib.

```
make            # kernel, programs, disk image, bootable ISO, all in build/
make run        # boot it in QEMU; the serial console mirrors to this terminal
make run-serial # serial only, no window
make test       # host unit tests, then the QEMU integration suite
make clean
```

The disk image (`build/disk.img`) is ordinary FAT16. It holds the
programs in `src/programs` plus `WELCOME.TXT`, and files you write in the OS persist on it
across boots. Running `make` again rebuilds it from scratch, so copy it
somewhere else if you want to keep what you wrote. `mdir -i build/disk.img`
and `fsck.fat -n build/disk.img` both read it.

## Testing

`make test` runs three suites.

`tests/test_heap_diskfs.c` builds the real kernel heap and filesystem
code as a 32-bit Linux program, on top of a RAM disk that can be told to
fail writes. It covers allocation and coalescing, double frees and bad
pointers, name rules and long names (aliases, ~N tails, case,
slots across sectors and clusters, bad checksums), sparse writes, truncation, full disks, root
directory exhaustion, an I/O error during a replace (the old contents
must survive), subdirectories (paths, growth past one cluster, rmdir
rules, remounts, the depth limit), a non-default cluster size and
malformed volumes. At the
end, fsck.fat checks the image the run left behind.

`tests/test_netmsg.c` builds the DHCP and DNS message code, the parts
that read bytes off the wire, under AddressSanitizer and UBSan. It
checks the DHCP client's messages byte by byte and the parser on offers,
NAKs, bad options and bad domains, then the DNS name rules, query
encoding and answer parsing: CNAME chains, compression (pointer loops,
forward pointers, names over 255 bytes), truncation, rcodes, and bytes
that must be escaped before a terminal sees them. Both parsers then eat
several hundred thousand mutated messages.

`tests/qemu_test.py` boots the real ISO in QEMU and drives the shell
over a serial socket. It uses `build/test-disk.img`, which adds the test
programs in `tests/programs` to the normal disk. It checks the boot
banner and runs every program, including the ones that are supposed to
crash. It covers bad pointers handed to syscalls, stack growth and
overflow, malloc, Ctrl+C on foreground jobs, background jobs, raw
terminal mode, ANSI output (read back from VGA memory through the QEMU
monitor), an editing session in Octavo, file and directory commands,
the timer rate,
the switch into graphics mode and back, reboot persistence and poweroff,
and the network through QEMU's user network: DHCP at boot, `ping`,
loopback, UDP and TCP sockets (`udptest`, `tcptest`), `fetch` and `nc`
against an HTTP server and an echo server on the host, `net static` and
`net dhcp`, and `host` against `tests/dns_server.py`, a DNS server on the host whose
names each misbehave in their own way (aliases, loops, SERVFAIL, silence,
forged answers, truncation, a compression loop, escape codes in names).
Afterwards it runs fsck.fat and mtools against the disk. Then it boots
again on a socket network whose only other host is `tests/net_peer.py`,
which puts raw Ethernet frames on the wire and plays the network's DHCP
and DNS server. It checks the DHCP messages field by field and a lease's
whole life (renewing, rebinding, running out, NAKs, lying servers, an
address set by hand), ARP and pings of every size and shape, packets
that must be ignored (bad checksums, fragments, other addresses,
broadcasts, short headers), UDP echoed by a guest program and port
unreachable for closed ports, the search domain and a second DNS server,
the guest's own pings answered, timed out, refused by a router or
damaged, and 4000 frames of garbage. Short boots check the other two
e1000 models and a machine with no card. Pass `--keep` to keep the
serial log and the disk.

## Design in brief

Memory: the kernel identity-maps the first 1 GB for itself, supervisor
only, with its code and read-only data write-protected. Each program gets
the range from 1 GB to 3 GB. Its image is mapped from the ELF segments
with their real permissions. The stack sits at the top and grows on
demand up to 1 MB, with an unmapped guard gap below it. The kernel heap
and the physical frame pool are sized from the Multiboot memory map. A program's heap is the break above its image, moved with
`sbrk`; the C runtime has malloc, calloc, realloc and free on top.

Tasks: a round-robin, preemptive scheduler runs on the 100 Hz PIT. The
kernel's own tasks (idle, init) and user programs are scheduled the same
way. init starts SAKLAS from the disk, hands it the terminal, and starts a
fresh one whenever it ends. A program that faults is killed and the fault is
logged (`faults` in the shell). A kill takes effect at the next safe
point, never in the middle of a kernel operation. The tty belongs to one
foreground job, which receives keyboard input, Ctrl+C and Ctrl+\. A job
can lend the terminal to a job it started and take it back once that job
is gone, getting its own terminal mode back with it; that is how the
shell runs a program in the foreground. It lends it as part of the spawn
(`SPAWN_FOREGROUND`), so the program has the terminal before its first
instruction rather than racing its parent for it.

Terminal: programs read the keyboard (or the serial port) through
standard input, a line at a time by default. Raw mode hands over every
key as it comes, with arrows, Home/End, PgUp/PgDn, Delete and F1-F12 as
xterm escape sequences and Alt as an ESC prefix; with TTY_NOSIG, Ctrl+C
is an ordinary byte, and Ctrl+\ is the kill switch that always works. `poll`
waits up to a given number of ticks for input, so a program can redraw on a
timer and still answer a key at once. The console understands the common ANSI escapes for cursor
motion, erasing, colours and hiding the cursor, on screen and on serial
alike. Opening `/dev/tty` (`DEV_TTY`) gives the terminal itself, read-only for
keys or write-only for output, whatever standard input and output are
redirected to; that is how `more` can wait for a key at the end of a pipe.

System calls: `int 0x80`, with the number in eax, arguments in ebx, ecx
and edx, and the result in eax. A failed call returns a negative errno.
Every user pointer is checked against the caller's page tables before the
kernel touches it. The numbers and structures are in
`src/include/ringtail/abi.h`, and the user-side wrappers are in
`src/programs/lib/ulib.h`. A spawned program inherits copies of its
parent's standard input, output and error (descriptors 0-2), and spawn
can remap them or put the program in a new or existing job. The shell is
an ordinary program: everything it shows comes through syscalls such as
`readdir`, `kinfo` (version, memory, heap, tasks, faults, memory map,
disk) and `kill`. Pipes are 4 KB kernel buffers: a reader waits for data
and sees end of file once every writer has closed, and a writer waits
for room. `dup2` copies a descriptor onto another number.

Filesystem: FAT16 with subdirectories and Windows-style long names
(VFAT). A name is up to 255 printable ASCII characters, none of
`\ / : * ? " < > |`, not starting with a space or ending in a space or
dot. Case is kept as typed and ignored when matching, so `cat readme.TXT`
finds `ReadMe.txt`. A name that fits 8.3 in one case per part is stored
as a plain short entry (lowercase via the NT case bits, like Windows);
any other gets long-name slots and a short alias such as `MYDOCU~1`,
which also works as its name. Disks written by Windows, Linux or mtools
read the same way. Paths use `/`, start at the task's current directory
unless they begin with `/`, and know `.` and `..`; a directory's full
path is at most 255 characters. Each task has
its own current directory, inherited by the programs it starts. The FAT
and the root directory are kept in memory, subdirectory sectors go
through a small cache, and all of it is written back after each
operation, FAT first. Files and directories are stamped with the CMOS clock when they are made and
when their contents change (`ls` shows it, `date` prints the clock; a rename
keeps it). A new directory's cluster is zeroed on disk before
anything points at it.
File data is always written before the metadata that points at it.
Replacing a file builds the new contents in fresh clusters before
switching the directory entry, so a failure leaves the old file intact;
programs get that through the `save` syscall.
Rename moves the entry (and a directory's `..`), so an open file stays
open through it, and a directory cannot be moved inside itself. An open
file cannot be deleted, and a directory that is some task's
current directory cannot be removed.

Network: PCI devices are found by scanning configuration space at boot
(`pci` lists them). The e1000 driver maps the card's registers into a
4 MB device window at 0xC0000000, above user space and in every address
space but supervisor only, and runs a 32-buffer receive ring and an
8-buffer transmit ring. Its interrupt only wakes the `net` kernel task,
which takes frames off the ring and handles them under one lock, so the
stack never runs in interrupt context. ARP learns neighbours from requests aimed at us and asks three times, a
second apart, before giving up; IPv4 checks lengths and checksums and
drops fragments; ICMP answers echo requests (not broadcast ones) and
matches replies and routers' unreachable messages to the `ping` syscall
waiting for them. Round trips are timed in microseconds from the PIT's
counter. Anything sent to 127/8 or the machine's own address goes round
through the net task without touching the card.

UDP sockets are file descriptors (`socket`, `bind`, `sendto`,
`recvfrom`; `read` and `write` refuse them). Binding port 0 picks a
random free port from 49152 up, port 68 is the DHCP client's, each
socket queues 16 datagrams and drops the rest, and a datagram for a
closed port gets ICMP port unreachable unless it was a broadcast. The
DHCP client (RFC 2131) runs in the net task: DISCOVER at boot, renewing
at T1 straight to the server, rebinding at T2 by broadcast, starting over
when the lease runs out or on a NAK, which it answers more slowly each
time. init gives it five seconds before starting the shell. `net static`
turns it off and sets the address by hand; `net dhcp` starts it again,
asking for the address DHCP gave last. The DNS resolver is user code
(`src/programs/lib/dns.c` over `dnsmsg.c`), linked into the programs
that look names up: it asks each server DHCP named in turn, twice, 2 s
each, from a random port with a random ID, believes an answer only from
that server's address and port with that ID and the same question,
follows up to 8 aliases, and tries a name with no dot under the search
domain first. Names that come back are escaped before they are printed.

TCP is split in two. `src/net/tcp.c` is the protocol engine and knows
nothing of the kernel: RFC 9293 with the MSS option (no window scaling,
timestamps or SACK), RFC 6298 retransmission timing, slow start and
congestion avoidance with NewReno fast recovery, zero-window probes,
delayed ACKs, out-of-order segments held until the gap fills, and the
RFC 5961 checks against blind resets and SYNs. Each connection buffers
16 KB each way; there are 64 at most, counting those still closing.
`tests/test_tcp.c` runs the engine on the host against a simulated
network that loses, reorders and duplicates segments.
`src/net/tcpsock.c` puts it in the kernel: the calls behind `connect`,
`listen`, `accept`, `shutdown` and the rest, all under the net lock,
with tasks sleeping on the connection and the wakeups sent once the
engine is done. TCP sockets read and write like files, and `pollfd`
waits on any mix of sockets, pipes and the terminal.

## Source layout

```
src/boot       Multiboot header, entry, kernel linker script
src/kernel     kernel_main, GDT/IDT/interrupts, tasks, syscalls, ELF loader, faults, power
src/mm         heap, physical frames, paging and checked user copies
src/drivers    console (VGA text + serial), tty, keyboard, mouse, timer, ATA, VGA graphics, PCI, e1000
src/net        ARP, IPv4, ICMP and loopback (net.c), UDP sockets (udp.c), TCP (tcp.c, tcpsock.c), the DHCP client (dhcp.c, dhcpmsg.c)
src/fs         FAT16 (diskfs.c) and file descriptors (file.c)
src/lib        string and printf subset, shared by the kernel and programs
src/include    kernel headers; ringtail/abi.h is the user/kernel contract
src/programs   user programs (the shell among them), their C library (lib/: runtime, malloc, DNS) and linker script
tools          mkdisk.py, the FAT16 image builder
tests          host unit tests, the QEMU integration suite, test programs
```

## Using it

Type `help` at the `ringtail:/>` prompt (it shows the current directory) for the command list. The editor
keeps the last 10 lines (Up and Down). Left, Right, Home, End, Delete and
Backspace work anywhere in the line, Ctrl+Left/Right (or Alt+B/F) jump a
word, and Ctrl+A/E/B/F do what Home/End/Left/Right do. Ctrl+W deletes the word before the cursor, Ctrl+K the rest of
the line and Ctrl+U the whole line. Ctrl+C cancels a line or kills the
foreground program, and Ctrl+L clears the screen. Tab (which works on the end of the line) completes commands,
programs and paths, ignoring case and quoting names with spaces; a second
Tab with several matches lists them.

`run <program> [args...]`, or just the program's name, starts a program
from the disk, and a trailing `&` runs it in the background.
`a | b` pipes one program's output into the next, `< FILE` feeds the
first command a file, and `> FILE` or `>> FILE` writes or appends the
last command's output. A built-in can start a pipeline (`ls | grep TXT`,
`help | grep file`) or be redirected (`echo hi > NOTE.TXT`), but not
come after a `|`. Ctrl+C stops every program in a pipeline. `tasks` lists what is running and
`kill <id>` stops it. `ls [dir]`, `cat <file>...` (several files in a row; into
a pipe or a file the bytes pass through exactly, so `cat a b > c` joins them), `touch`, `write <file> <text>`
and `rm` work on files, and `cd`, `pwd`, `mkdir` and `rmdir` on
directories. `mv` renames or moves a file or a whole directory (into a
directory if the target is one), and the `cp` program copies files. Every file argument can be a path (`docs/a.txt`, `"My Docs/notes.txt"`,
`../B.TXT`, `/WELCOME.TXT`). A bare program name is always looked up in
`/`, wherever you are; give a path to run one from somewhere else. `mem`, `heap` and `mmap` show memory, `date` the clock, `uptime` the
time since boot and timer ticks, and `faults` the fault log. `pci` lists the PCI devices, and `net` shows
the network card, the address and where it came from, the DNS servers,
traffic counts, UDP sockets and the ARP table. `net static
10.0.2.20/24 10.0.2.2 10.0.2.3` sets the address, gateway (or `none`) and
DNS server by hand, and `net dhcp` goes back to DHCP. `color <name>` sets
the text colour, and `reboot` and `poweroff` do what they say. `exit`
ends the shell, and init starts a new one.

Programs on the disk:

| Program | What it does |
|---|---|
| octavo | Text editor. `run octavo [file]`. |
| kenoma | Desktop: a paged launcher for every program, and page icons for .txt, .lemur and .lem files that open them in Octavo. Esc quits. |
| saklas | The command line (the shell). init starts it at boot and again whenever it ends. |
| gauge | System monitor: memory, disk and every task, redrawn each second. q quits, p pauses, + and - change the rate. `gauge -1` prints one plain snapshot, so it pipes. |
| ping | `ping [-c COUNT] [-s SIZE] [-W MS] HOST`: ICMP echo, a second apart, with round trips and a summary. HOST is an address or a name. |
| host | `host [-p PORT] NAME [SERVER]`: asks DNS about a name, or about an address for its name, and prints the answer the way BIND's `host` does. |
| fetch | `fetch [-i] [-o FILE] URL`: gets a page over HTTP/1.0, following up to five redirects. -i shows the headers, -o saves the body. No https. |
| httpd | `httpd [-p PORT] [-q] [DIR]`: serves DIR (default: the current directory) over HTTP on port 8080, one connection at a time. A directory gives its index.html or a listing; `..` is refused. Logs each request unless -q. Ctrl+C stops it. |
| nc | `nc [-C] [-v] [-w SECS] HOST PORT` or `nc -l PORT`: a TCP connection between the terminal (or stdin) and the other end. -C sends CR LF line ends. |
| sleep | `sleep <seconds>`, up to an hour. |
| cp | `cp SRC DST` or `cp SRC... DIR`: copies files, replacing an existing target. |
| wc | Counts lines, words and bytes of files, or of its input. |
| grep | `grep [-i] TEXT [files]`: the lines containing TEXT (plain text, not patterns). |
| head | `head [-n N \| -c N] [-q] [FILE...]`: the first 10 lines of each file, or of its input; `-N` is `-n N`. Stops reading once it has enough. |
| tail | `tail [-n [+]N \| -c [+]N] [-q] [FILE...]`: the last 10 lines, or everything from line N on with `+N`. Reads to the end, keeping only what it needs. No -f. |
| sort | `sort [-bcfnrsu] [-k F1[,F2]] [-t SEP] [-o FILE] [FILE...]`: sorts lines bytewise, or as numbers with -n, backwards with -r, ignoring case with -f. -u drops repeats, -c only checks, -o may name an input. One -k key, with the usual field rules. Ties go by the whole line unless -s or -u. Holds all its input in memory. |
| uniq | `uniq [-c \| -d \| -u] [-i] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]`: collapses runs of equal lines, with -c counts, -d only the repeated, -u only the single. Streams, so it works on any size; run `sort` first. |
| hexdump | `hexdump [-C \| -x] [-v] [-n LENGTH] [-s OFFSET] [FILE...]`: the bytes of the files as one stream, as 16-bit words (default, `-x`) or canonical hex and text (`-C`). A repeated line becomes `*` unless -v. -n and -s take hex. |
| df | `df [-h] [-k] [FILE...]`: size, used and free space on the disk, in 1K blocks or with -h in K, M and G. |
| more | `more [FILE...]`: one screenful at a time, with a percentage for plain files. Space or PgDn for a screen, Enter or Down for a line, q or Ctrl+C to quit. Reads keys from `/dev/tty`, so it works on a pipe; when output is not the screen it behaves like cat. |
| hello | Hello world, and the starting point for your own |

To write your own, copy `src/programs/hello.c`. Every `.c` file in
`src/programs` is built and put on the disk automatically, under its name
in uppercase. Test programs go in `tests/programs` and land only on the
test disk.

### Octavo

A port of [Octavo](https://github.com/tdotlab/octavo), a small Win32
editor, to the RingTail terminal. Word wrap is on by default; Alt+Z
toggles it.

| Action | Keys |
|---|---|
| New / Open / Save | Ctrl+N / Ctrl+O / Ctrl+S |
| Save as hylic | F12 |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Find / Replace | Ctrl+F / Ctrl+R (Tab toggles match case) |
| Find next / previous | F3 / Shift+F3 |
| Cut / Copy / Paste / Select all | Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+A |
| Move or delete by word | Ctrl+Left/Right, Ctrl+Backspace/Delete |
| Start / end of document | Ctrl+Home / Ctrl+End |
| Quit | Ctrl+Q |

Differences from the Windows version: Replace is Ctrl+R because Ctrl+H
is Backspace in a terminal, and Save as hylic is F12 because Ctrl+Shift+S
arrives as Ctrl+S. New documents are saved as `.lemur`. Files keep their line endings (LF or CR LF). Bytes are shown
one column each, so it is an ASCII editor; there is no mouse, and files
are limited to 1 MB. If it ever hangs, Ctrl+\ ends it.

## Limits

These are deliberate, or simply not done yet. Programs get the x87 FPU
(its state is saved per task) but not SSE. Long names are ASCII only (a name
with other characters shows as its short alias). The clock is the CMOS
one, with no time zone (UTC under QEMU) and no changes for summer time. There is no
`fork`/`exec` (programs start other programs with `spawn`), no
signals beyond kill (so a program writing to a pipe nobody reads gets
EPIPE back instead of being stopped), and no SMP. The only display modes are VGA text and
mode 13h. The network has no TCP and no IPv6, so DNS answers too big
for UDP are an error. The resolver keeps no cache, nothing reads ICMP
port unreachable for a socket (a dead DNS server is found out by
timeout), and the DHCP client does not check that nobody else already
has the address it was given (RFC 5227). The e1000 is the only card it
drives.

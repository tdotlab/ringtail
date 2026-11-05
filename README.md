# RingTail OS

A small 32-bit x86 operating system written from scratch in freestanding
C and NASM. GRUB boots it through Multiboot. It runs ELF programs from a
FAT16 disk in separate, protected address spaces, and it has a preemptive
scheduler, a shell, a text editor, and a mouse-driven desktop in VGA mode
13h.

Current version: **v0.19.0 "Timekeeper"**. See PROGRESS.md for the history.

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

`make test` runs two suites.

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

`tests/qemu_test.py` boots the real ISO in QEMU and drives the shell
over a serial socket. It uses `build/test-disk.img`, which adds the test
programs in `tests/programs` to the normal disk. It checks the boot
banner and runs every program, including the ones that are supposed to
crash. It covers bad pointers handed to syscalls, stack growth and
overflow, malloc, Ctrl+C on foreground jobs, background jobs, raw
terminal mode, ANSI output (read back from VGA memory through the QEMU
monitor), an editing session in Octavo, file and directory commands,
the timer rate,
the switch into graphics mode and back, reboot persistence and poweroff.
Afterwards it runs fsck.fat and mtools against the disk. Pass `--keep`
to keep the serial log and the disk.

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
way. init starts SHELL from the disk, hands it the terminal, and starts a
fresh one whenever it ends. A program that faults is killed and the fault is
logged (`faults` in the shell). A kill takes effect at the next safe
point, never in the middle of a kernel operation. The tty belongs to one
foreground job, which receives keyboard input, Ctrl+C and Ctrl+\. A job
can lend the terminal to a job it started and take it back once that job
is gone, getting its own terminal mode back with it; that is how the
shell runs a program in the foreground.

Terminal: programs read the keyboard (or the serial port) through
standard input, a line at a time by default. Raw mode hands over every
key as it comes, with arrows, Home/End, PgUp/PgDn, Delete and F1-F12 as
xterm escape sequences and Alt as an ESC prefix; with TTY_NOSIG, Ctrl+C
is an ordinary byte, and Ctrl+\ is the kill switch that always works. The console understands the common ANSI escapes for cursor
motion, erasing, colours and hiding the cursor, on screen and on serial
alike.

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

## Source layout

```
src/boot       Multiboot header, entry, kernel linker script
src/kernel     kernel_main, GDT/IDT/interrupts, tasks, syscalls, ELF loader, faults, power
src/mm         heap, physical frames, paging and checked user copies
src/drivers    console (VGA text + serial), tty, keyboard, mouse, timer, ATA, VGA graphics
src/fs         FAT16 (diskfs.c) and file descriptors (file.c)
src/lib        string and printf subset, shared by the kernel and programs
src/include    kernel headers; ringtail/abi.h is the user/kernel contract
src/programs   user programs (the shell among them), their C runtime (lib/) and linker script
tools          mkdisk.py, the FAT16 image builder
tests          host unit tests, the QEMU integration suite, test programs
```

## Using it

Type `help` at the `ringtail:/>` prompt (it shows the current directory) for the command list. The editor
keeps the last 10 lines (Up and Down), Ctrl+C cancels a line or kills the
foreground program, and Ctrl+L clears the screen. Tab completes commands,
programs and paths, ignoring case and quoting names with spaces; a second
Tab with several matches lists them.

`run <program> [args...]`, or just the program's name, starts a program
from the disk, and a trailing `&` runs it in the background.
`a | b` pipes one program's output into the next, `< FILE` feeds the
first command a file, and `> FILE` or `>> FILE` writes or appends the
last command's output. A built-in can start a pipeline (`ls | grep TXT`,
`help | grep file`) or be redirected (`echo hi > NOTE.TXT`), but not
come after a `|`. Ctrl+C stops every program in a pipeline. `tasks` lists what is running and
`kill <id>` stops it. `ls [dir]`, `cat`, `touch`, `write <file> <text>`
and `rm` work on files, and `cd`, `pwd`, `mkdir` and `rmdir` on
directories. `mv` renames or moves a file or a whole directory (into a
directory if the target is one), and the `cp` program copies files. Every file argument can be a path (`docs/a.txt`, `"My Docs/notes.txt"`,
`../B.TXT`, `/WELCOME.TXT`). A bare program name is always looked up in
`/`, wherever you are; give a path to run one from somewhere else. `mem`, `heap` and `mmap` show memory, `date` the clock, `uptime` the
time since boot and timer ticks, and `faults` the fault log. `color <name>` sets
the text colour, and `reboot` and `poweroff` do what they say. `exit`
ends the shell, and init starts a new one.

Programs on the disk:

| Program | What it does |
|---|---|
| octavo | Text editor. `run octavo [file]`. |
| gui | Desktop: a paged launcher for every program, and page icons for .txt, .lemur and .lem files that open them in Octavo. Esc quits. |
| shell | The command line. init starts it at boot and again whenever it ends. |
| sleep | `sleep <seconds>`, up to an hour. |
| cp | `cp SRC DST` or `cp SRC... DIR`: copies files, replacing an existing target. |
| wc | Counts lines, words and bytes of files, or of its input. |
| grep | `grep [-i] TEXT [files]`: the lines containing TEXT (plain text, not patterns). |
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
mode 13h.

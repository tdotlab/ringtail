# RingTail OS

A small 32-bit x86 operating system written from scratch in freestanding
C and NASM. GRUB boots it through Multiboot. It runs ELF programs from a
FAT16 disk in separate, protected address spaces, and it has a preemptive
scheduler, a shell, and a mouse-driven desktop in VGA mode 13h.

Current version: **v0.11.0 "Hardened"**. See PROGRESS.md for the history.

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

The disk image (`build/disk.img`) is ordinary FAT16. It holds every
program plus `WELCOME.TXT`, and files you write in the OS persist on it
across boots. Running `make` again rebuilds it from scratch, so copy it
somewhere else if you want to keep what you wrote. `mdir -i build/disk.img`
and `fsck.fat -n build/disk.img` both read it.

## Testing

`make test` runs two suites.

`tests/test_heap_diskfs.c` builds the real kernel heap and filesystem
code as a 32-bit Linux program, on top of a RAM disk that can be told to
fail writes. It covers allocation and coalescing, double frees and bad
pointers, 8.3 name rules, sparse writes, truncation, full disks, root
directory exhaustion, an I/O error during a replace (the old contents
must survive), a non-default cluster size and malformed volumes. At the
end, fsck.fat checks the image the run left behind.

`tests/qemu_test.py` boots the real ISO in QEMU and drives the shell
over a serial socket. It checks the boot banner and runs every program,
including the ones that are supposed to crash. It covers bad pointers
handed to syscalls, stack growth and overflow, Ctrl+C on foreground jobs,
background jobs, the line editor, file commands, the timer rate, the
switch into graphics mode and back, reboot persistence and poweroff.
Afterwards it runs fsck.fat and mtools against the disk. Pass `--keep`
to keep the serial log and the disk.

## Design in brief

Memory: the kernel identity-maps the first 1 GB for itself, supervisor
only, with its code and read-only data write-protected. Each program gets
the range from 1 GB to 3 GB. Its image is mapped from the ELF segments
with their real permissions. The stack sits at the top and grows on
demand up to 1 MB, with an unmapped guard gap below it. The kernel heap
and the physical frame pool are sized from the Multiboot memory map.

Tasks: a round-robin, preemptive scheduler runs on the 100 Hz PIT. The
kernel's own tasks (the shell, the demo tasks) and user programs are
scheduled the same way. A program that faults is killed and the fault is
logged (`faults` in the shell). A kill takes effect at the next safe
point, never in the middle of a kernel operation. The tty belongs to one
foreground job, which receives keyboard input and Ctrl+C.

System calls: `int 0x80`, with the number in eax, arguments in ebx, ecx
and edx, and the result in eax. A failed call returns a negative errno.
Every user pointer is checked against the caller's page tables before the
kernel touches it. The numbers and structures are in
`src/include/ringtail/abi.h`, and the user-side wrappers are in
`src/programs/lib/ulib.h`.

Filesystem: FAT16, root directory only, DOS 8.3 names. The FAT and the
directory are cached in memory and written back after each operation.
File data is always written before the metadata that points at it.
Replacing a file builds the new contents in fresh clusters before
switching the directory entry, so a failure leaves the old file intact.
An open file cannot be deleted.

## Source layout

```
src/boot       Multiboot header, entry, kernel linker script
src/kernel     kernel_main, GDT/IDT/interrupts, tasks, syscalls, ELF loader, faults, power
src/mm         heap, physical frames, paging and checked user copies
src/drivers    console (VGA text + serial), tty, keyboard, mouse, timer, ATA, VGA graphics
src/fs         FAT16 (diskfs.c) and file descriptors (file.c)
src/lib        string and printf subset, shared by the kernel and programs
src/shell      the kernel shell
src/include    kernel headers; ringtail/abi.h is the user/kernel contract
src/programs   user programs, their C runtime (lib/) and linker script
tools          mkdisk.py, the FAT16 image builder
tests          host unit tests and the QEMU integration suite
```

## Using it

Type `help` at the `ringtail>` prompt for the command list. The editor
keeps the last 10 lines (Up and Down), Ctrl+C cancels a line or kills the
foreground program, and Ctrl+L clears the screen.

`run <program> [args...]` starts a program from the disk, and a trailing
`&` runs it in the background. `tasks` lists what is running and
`kill <id>` stops it. `ls`, `cat`, `touch`, `write <file> <text>` and
`rm` work on files. `mem`, `heap` and `mmap` show memory, and `faults`
shows the fault log. `gfx` is a graphics demo, and `reboot` and
`poweroff` do what they say.

Programs on the disk:

| Program | What it does |
|---|---|
| gui | Desktop: draggable windows and a paged launcher for every program. Esc quits. |
| hello, args, count | Hello world, argument echo, a counter that sleeps between lines |
| readline, waitdemo | Read a line from the terminal; sleep until keyboard or mouse input |
| filedemo, seekdemo, listdemo | Self-checking tests of the file, seek and directory syscalls |
| badptr, kptr, nullds, bss, stack | Self-checking tests: bad and kernel pointers rejected, loaded segments correct, stack growth |
| crash, fault, gfxfault, `stack overflow` | Programs that die on purpose, to show the kernel survives them |

To write your own, copy `src/programs/hello.c`. Every `.c` file in
`src/programs` is built and put on the disk automatically, under its name
in uppercase.

## Limits

These are deliberate, or simply not done yet. Programs get the x87 FPU
(its state is saved per task) but not SSE. There are no subdirectories or long file names, no
`fork`/`exec` (programs start other programs with `spawn`), no pipes, no
signals beyond kill, and no SMP. The only display modes are VGA text and
mode 13h.

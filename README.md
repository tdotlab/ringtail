# RingTail OS

Bare-metal x86 operating system, built from scratch: Multiboot/GRUB boot,
freestanding C + NASM, no libc.

Current version: v0.9.0 - GUI

- Boot, VGA print/scroll/hardware cursor
- IDT + PIC remap + keyboard with shell history
- Heap free-list allocator with coalescing (kmalloc/kfree)
- Disk filesystem (RTFS): flat, contiguous files on an ATA disk,
  shell commands ls, touch, cat, write, rm, files persist across
  boots with the same disk image
- Paging: per-program address spaces, kernel pages
  supervisor-only, programs load at 0x08000000 from disk
- Programs: hello, count, fault, crash, badptr, filedemo, gui as
  separate binaries on disk, run with `run <name>`, via int 0x80
  syscalls (print, sleep, exit, open, read, write, close, delete,
  graphics begin/end/pixel/clear/rect/line/text, mouse state, key get)
- PS/2 mouse (IRQ 12) with `mouse` command; VGA mode 13h graphics
  (320x200, 256 colors) with `gfx` demo: shapes, text, cursor that
  follows the mouse, click to draw, any key returns to the text shell
- GUI desktop (run with `run gui`): top bar with Quit, two icons,
  two draggable windows with buttons, Esc or Quit returns to the shell
- Colors: print_color, `color <name>`, `colors` palette demo, errors in red,
  successes in green, `clear` resets the color
- Timer: PIT at ~100 Hz, `uptime`, `tick`, `sleep <seconds>`
- v0.2.3 audit fixes: exception handlers, PIC masking, validated free,
  allocate-before-free RAMFS writes, shift/caps/E0 keyboard handling,
  real hex addresses, strict number parsing, own GDT, BSS clearing,
  multiboot magic and RAM checks (free now takes a hex address, as
  printed by malloc)
- v0.3.0 architectural cleanup: kmalloc rejects sizes that would
  overflow alignment arithmetic; timer_sleep uses wrap-safe elapsed
  ticks and rejects durations over 21474836 seconds; the keyboard
  ISR only enqueues scancodes and the kernel main loop runs the
  shell, so input during sleep is typeahead instead of dropped;
  host tests for heap and RAMFS live in tests/ (make run, needs
  gcc -m32)

Build (from this folder):

```
make run
```

Requires i686-elf-gcc, nasm, grub-mkrescue, and qemu-system-i386.

Host tests (heap and disk filesystem only; timer, keyboard, ATA,
and paging use port I/O or control registers and are QEMU-only):

```
cd tests
make run
```

Needs gcc -m32 (gcc-multilib on Ubuntu).

History: earlier versions (v0.0.7 through v0.2.0) were tracked as concatenated
source documents in Google Drive ("RingTail OS - Version History"), because
that was all the Meta AI app had available. From v0.2.1 on, this repo is the
home of the source. The Drive docs are kept as an archive.

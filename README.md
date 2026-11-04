# RingTail OS

Bare-metal x86 operating system, built from scratch: Multiboot/GRUB boot,
freestanding C + NASM, no libc.

Current version: v0.2.2 - Timer

- Boot, VGA print/scroll/hardware cursor
- IDT + PIC remap + keyboard with shell history
- Heap free-list allocator with coalescing (kmalloc/kfree)
- RAMFS: files live in the heap, shell commands ls, touch, cat, write, rm
- Colors: print_color, `color <name>`, `colors` palette demo, errors in red,
  successes in green, `clear` resets the color
- Timer: PIT at ~100 Hz, `uptime`, `tick`, `sleep <seconds>`

Build (from this folder):

```
make run
```

Requires i686-elf-gcc, nasm, grub-mkrescue, and qemu-system-i386.

History: earlier versions (v0.0.7 through v0.2.0) were tracked as concatenated
source documents in Google Drive ("RingTail OS - Version History"), because
that was all the Meta AI app had available. From v0.2.1 on, this repo is the
home of the source. The Drive docs are kept as an archive.

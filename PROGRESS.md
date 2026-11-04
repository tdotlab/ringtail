# RingTail

v0.0.1 - Multiboot + stack
v0.0.2 - one char
v0.0.3 - print()
v0.0.4 - IDT + keyboard
v0.0.5 - shell buffer
v0.0.6 - history
v0.0.7 - scrolling + cursor
v0.1.0 - heap bump
v0.1.1 - heap free list
v0.2.0 - RAMFS (NEW)

Current (v0.2.0):
- ramfs.c NEW: file_t {name[32], size, data*, next}, file_list linked list in heap
- ramfs_init, create, write, read, delete, list, exists
- kernel.c calls ramfs_init()
- shell commands: ls, touch <file>, cat <file>, write <file> <text>, rm <file>
- Uses kmalloc/kfree for file structs and data

Test:
touch hello.txt
write hello.txt Hello RingTail!
ls
cat hello.txt
rm hello.txt

v0.2.1 - Colors + TUI
- kernel.c: cur_color, set_color/get_color, print_color, colored prompt/banner, clear resets color
- shell.c: color <name>, colors palette demo, errors red, successes green
- linker.ld: _kernel_end symbol
- Makefile: link all objects, not just boot+kernel

Current (v0.2.1):
- Colors: print_color, color command, colors demo
- TUI: colored prompt, banner, error/success coloring, clear resets color

Test:
colors
color lightred
echo hello (red)
clear (back to white)
touch hello.txt (Created in green)
cat nope (red error)

v0.2.2 - Timer
- timer.c NEW: PIT channel 0 at ~100 Hz (divisor 11931), volatile tick counter, timer_handler with EOI to master PIC
- isr.asm: timer_isr wrapper, idt.c: gate 32 for IRQ0, kernel.c calls timer_init()
- shell commands: uptime, tick, sleep <seconds>
- Wrinkle: the shell runs inside the keyboard ISR with interrupts off, so timer_sleep does sti, halts until the target tick, then cli again; keyboard.c drops keys while timer_sleeping is set, so a keypress during sleep cannot re-enter the shell

Current (v0.2.2):
- Timer: PIT ticks, uptime in seconds, raw tick count, sleep
- Everything from v0.2.1 still works: heap, RAMFS, colors

Test:
uptime
tick
sleep 2
uptime (should be about 2 seconds larger)

v0.2.3 - Audit fixes (full code audit, 2026-11-04)
- idt.c: PIC masks 0xF8/0xFF (only timer, keyboard, cascade unmasked);
  exception handlers for vectors 0-31 that print the vector and halt
  instead of triple-faulting with no diagnostic
- isr.asm: exception stubs 0-31 via macro, recording the vector in
  exc_vector; the common handler never irets, so CPU error codes
  pushed for some vectors are harmless
- heap.c: kfree now returns 0/-1/-2 and only dereferences a pointer
  after finding it in the free list; block header padded to 16 bytes
  for 8-byte alignment; heap_get_total added
- ramfs.c: write allocates the new data before freeing the old, so
  out-of-memory no longer leaves a dangling pointer; read guards max==0
- keyboard.c: E0 extended-key state machine, shift (left/right) and
  caps lock with shifted symbols, tab ignored instead of printing a
  VGA glyph
- shell.c: real hex addresses (print_hex) and free takes hex as
  printed by malloc; strict decimal/hex parsers (no more silent 0 for
  garbage input); touch/cat/rm/write report the real failure reason
  (exists, too long, empty name, out of memory); heap shows total
- boot.asm: own GDT, multiboot magic check (red B on failure), BSS
  clearing, passes magic + info pointer to kernel_main
- kernel.c: verifies multiboot magic, requires at least 5 MB above
  1 MB for the 4 MB heap when the info flags give a size
- linker.ld: _bss_start/_bss_end symbols; timer.c: divisor 11932
  (99.998 Hz, closer than 11931); Makefile: -fno-builtin
  -fno-stack-protector so -O2 cannot invent memcpy/memset calls
- Not changed: heap split/coalesce logic, RAMFS create/read/delete/
  list, IDT remap sequence, VGA scroll/cursor, timer sleep coupling
  (still runs inside the keyboard ISR; a scheduler is what fixes that)

Test:
malloc 64
free <hex address printed by malloc>
free 123 (should report Invalid address, not corrupt the heap)
touch averyveryveryveryveryveryverylongname (should report too long)
sleep abc (should report Usage, not sleep 0)
Shift + a (should print A), Caps Lock + a (should print A)

v0.3.0 - Input queue + overflow fixes + host tests
- heap.c: kmalloc rejects size > max usable before aligning, and
  sizes whose alignment arithmetic would wrap past 2^32; malloc
  4294967295 now returns NULL instead of a wrapped 0-byte block
- timer.c: timer_sleep returns int, rejects seconds > 21474836
  (2^31 ticks), and waits on wrap-safe elapsed ticks (ticks -
  start) instead of an absolute target; timer_sleeping removed
- keyboard.c: ISR only reads a scancode, enqueues it in a 256-entry
  ring (head written by ISR, tail by main loop, full queue drops
  rather than overwrites), and sends EOI; keyboard_poll and
  keyboard_process_scancode (the v0.2.3 state machine, unchanged
  in behavior) run in the main loop
- kernel.c: main loop drains the input queue, then halts; the PIT
  tick every 10 ms guarantees a wakeup, so there is no lost-wakeup
  deadlock; sleep in the main loop needs no sti/cli dance, and
  keys typed during sleep become typeahead instead of being dropped
- shell.c: sleep reports "Duration too large" for rejected
  durations; version v0.3.0
- tests/ NEW: test_heap_ramfs.c (60 checks: alloc/free, coalescing,
  invalid/double free, overflow rejections, RAMFS create/write/
  read/delete, OOM write preserving old content) plus Makefile
  (gcc -m32, make run). Host build needs gcc-multilib; the test
  binary itself has not yet been run on this box for that reason.
  Timer/keyboard are QEMU-only by design (port I/O faults in
  user mode).
- Not changed, deliberately: heap split/coalesce logic, RAMFS
  create/read/delete/list, memory-map-based heap placement (the
  kernel still checks mem_upper and the heap still assumes 4 MB
  contiguous after the kernel; that is a real-hardware concern,
  not a QEMU one, and it stays open)

Test:
malloc 4294967295 (should report Out of memory, not allocate)
sleep 42949673 (should report Duration too large)
sleep 2, type during sleep (keys should appear after, as typeahead)
cd tests && make run (once gcc-multilib is installed)

v0.3.1 - Boot fixes (found by actually building and booting in QEMU)
- boot.asm: save the multiboot magic into esi BEFORE loading the
  GDT, because mov ax, 0x10 in the segment reload clobbered the
  low 16 bits of eax and kernel_main halted with "Bad multiboot
  magic" (v0.3.0 as pushed would not boot at all); .bad_boot
  renamed to bad_boot because a dot-label after reload_cs belonged
  to reload_cs, not to _start, and NASM refused to assemble it
- Makefile: grub.cfg generation changed from a multi-line echo
  (invalid make: only the first line had a tab, so make failed
  with "missing separator" before building anything) to a single
  printf line
- boot.asm, isr.asm: .note.GNU-stack section, silencing the ld
  warning about an executable stack (irrelevant on bare metal,
  but the link is now clean)
- tests/Makefile: run target falls back to qemu-i386 if the native
  32-bit binary cannot run (on this VM even a trivial 32-bit
  binary segfaults, loader and static alike, so native 32-bit
  execution is unavailable here; qemu-i386 runs the tests fine)
- Host tests: all 60 checks pass under qemu-i386. QEMU system
  boot: v0.3.0 with the boot fix boots to a working shell;
  version, heap (start 0x10c000, total 4194304, free 4194288),
  touch/write/cat/ls, echo with shift, malloc overflow, sleep
  overflow, sleep 2 with typeahead uptime, and up-arrow history
  all verified from screendumps.

Test:
make myos.bin CC=gcc 'LD=ld -m elf_i386' CFLAGS='-ffreestanding -O2 -Wall -Wextra -nostdlib -lgcc -fno-builtin -fno-stack-protector -m32'
qemu-system-i386 -kernel myos.bin -m 16 -display none -monitor unix:/tmp/ringtail-mon,server,nowait

v0.3.2 - Layout (no behavior change)
- Sources moved from a flat ringtail/ into src/ by subsystem:
  src/boot (boot.asm, linker.ld), src/kernel (kernel.c, idt.c,
  isr.asm), src/drivers (keyboard.c, timer.c), src/mm (heap.c),
  src/fs (ramfs.c), src/shell (shell.c). tests/ unchanged.
  Makefile object list and linker path updated, tests Makefile
  paths updated. No source file includes another, so the move is
  mechanical; rebuild, host tests, and QEMU boot re-verified
  after the move.
- Build artifacts (.o, myos.bin, myos.iso, isodir) are not part
  of the source tree; make clean removes them.

v0.4.0 - Cooperative tasks (no behavior change to anything else)
- New src/kernel/task.c + task.asm: a task is a function with its
  own 4096-byte heap stack. switch_to (asm) saves only the
  callee-saved registers and the stack pointer, because a switch
  only happens at a yield point, never mid-expression. A new
  task's stack is faked to look like a task that just called
  switch_to, so its first switch "returns" into a trampoline
  that runs its entry function and then exits it.
- Scheduler: round-robin over a linked list, main loop is task 0
  ("shell"). task_yield picks the next ready task, task_sleep
  blocks until a wrap-safe wake tick, task_exit marks dead and
  the main loop's task_cleanup frees the stack. No locks: with
  no preemption, a print or heap operation is never interrupted
  by a task switch.
- Shell: tasks (list), spawn counter (demo task counting once a
  second), kill <id>. Known limitation: shell `sleep` still uses
  timer_sleep and blocks the scheduler while it runs; task_sleep
  is for tasks only.
- Test: spawn counter, watch it count while typing, tasks, kill,
  then tasks again; host tests still pass, QEMU boot re-verified.

Next: scheduler / multitasking, cooperative task system first,
then timer-driven preemption (the shell is now out of interrupt
context, so the foundation for tasks is clean)

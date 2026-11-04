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

v0.5.0 - Preemptive tasks
- timer.c: timer_handler now calls task_preempt() on every tick
  (100 Hz), after sending the PIC EOI, so the next tick can fire
  while the newly switched-in task runs. EOI before the switch
  matters: EOI after the switch would stall ticks until the old
  task resumed.
- task.c: task_preempt() mirrors task_yield, but the timer, not
  the task, decided to switch, so the current task may be mid-loop,
  mid-print, or mid-sleep. Reusing switch_to from an interrupt
  handler works because the interrupted task's interrupt frame
  (timer_isr's pusha registers plus the CPU's EFLAGS/CS/EIP) stays
  parked on its stack underneath the switch_to frame, and is
  unwound by popa/iret when that task is switched back to.
- task.c: critical sections use cli/sti, not a lock counter.
  task_yield, task_preempt, task_create, task_kill, and
  task_cleanup all cli before touching the task list or picking
  the next task, and sti after the switch resumes, so each task
  restores its own interrupt state. A lock counter would be
  wrong here: a yield's lock stays held across the switch and is
  only released by the incoming task's resume, so the count would
  hover above zero forever and preemption would never fire.
- isr.asm: timer_isr now does sti before calling timer_handler
  (so the switched-in task resumes with interrupts on) and cli
  before popa/iret (so the restore is not itself interrupted).
  The keyboard ISR is unchanged: short, no switch, interrupts
  stay off for its run.
- Bug found by actually testing, not by reading: the first
  v0.5.0 build froze the moment a busy task was spawned, because
  switch_to is called with interrupts off and a fresh task's
  trampoline inherited that state. A task that never yields
  (busy) then spun forever with the timer and keyboard masked,
  so no preemption could ever reach it. Fix: task_trampoline
  does sti before calling the entry function, so every new task
  starts with interrupts on. Existing tasks already restore
  interrupts via the sti after their yield/preempt switch.
- Shell: spawn busy (a task that spins forever, never yields,
  never prints; under cooperative scheduling it would freeze the
  OS, under preemption it is harmless and the shell stays
  responsive). version v0.5.0.
- Side effect, and an improvement: shell `sleep` no longer
  blocks other tasks. In v0.4.0 it did, because nothing switched
  during the sleep loop. Now the timer preempts the sleeping main
  task on every tick, so counter and busy keep running during a
  shell sleep, and the sleep still ends on time.
- Known limits, stated plainly: heap, RAMFS, and VGA output are
  NOT protected by cli/sti here. That is safe for the tasks that
  exist (shell is the only task touching heap/RAMFS; counter and
  busy only print, sleep, or spin), but a future task that
  allocates or edits files while the shell does the same would
  need the same protection or it could corrupt the free list or
  the file list. Prints are no longer atomic either: a task can
  be preempted mid-print, so output can interleave mid-line
  (seen in testing: "tacounter: 5"). A print lock is the obvious
  next hardening step.
- Test (QEMU, -kernel and GRUB ISO boots both reach the shell;
  host tests still pass): tasks, spawn busy, tasks (shell
  responsive, busy ready), echo hello, sleep 2 (completes on
  time despite busy), spawn counter (counts 0-4 alongside busy),
  tasks (shell running, busy ready, counter sleeping), kill 1
  (busy stops, counter keeps counting), tasks, kill 2, tasks
  (shell only, no further counter output). Interleaving seen
  and expected; no corruption, no freeze.

v0.5.1 - Preemption hardening (not 1.0: versioning rule is that
1.0 waits for a significant milestone, and this is a correctness
pass, not a milestone)
- kernel.c: VGA print lock, a recursive spinlock (xchg acquire,
  pause spin, owner = current task id, count). print() itself
  does NOT lock: a counter line is three print calls, and
  per-call locking would still interleave between them. Instead
  a logical output unit locks once: the counter locks around its
  whole line, the shell locks around a whole command's output
  (shell_enter, newline through next prompt). Command output is
  now atomic; the v0.5.0 "tacounter: 5" split inside command
  output is gone.
- Why a spinlock, not cli/sti, for prints: the shell's sleep
  branch releases the print lock before waiting and takes it
  again after, so the counter keeps printing during a sleep and
  ticks keep flowing. A cli lock held across a sleep would mask
  the timer and deadlock the sleep. A spinning task is
  preemptible, so the lock holder always gets slices to finish
  and release; no deadlock as long as holders run to unlock,
  which both holders here do. Recursive because print_color
  calls print and the sleep branch unlocks/relocks inside an
  outer lock; a plain flag would deadlock on the second acquire.
- Typed input echo is deliberately NOT locked: shell_putchar
  prints one character per key, and holding the lock across a
  typing session would stall the counter while the user pauses.
  Typed characters can still interleave with counter output in
  the display, and that is normal terminal behavior; the shell
  still reads the typed command correctly, and command output
  itself is clean. Do not mistake echo interleaving for a
  failed print lock.
- task.c: task_current_id() for the print lock's owner check
  (-1 before task_init, which never locks).
- idt.c: exception_handler breaks the print lock before
  printing, or a fault mid-print would spin forever on a lock
  held by a task that will never run again.
- heap.c, ramfs.c: interrupt save/restore around kmalloc,
  kfree, heap free-list walks, and every public ramfs function
  (single return points, as in task_create). Save/restore nests
  safely, unlike bare cli/sti, which would re-enable too early
  on an inner restore. The heap wrapper form (impl + wrapper)
  covers every early return by construction. The RINGTAIL_KERNEL
  guard is load-bearing: cli/sti fault in user mode, so host
  tests compile them out as no-ops (correct there, since host
  tests are single-threaded); kernel builds define it via the
  Makefile CFLAGS, and a kernel build forgetting it would
  silently lose heap/RAMFS locking.
- Makefile: CFLAGS now includes -DRINGTAIL_KERNEL.
- Test (QEMU, -kernel and GRUB ISO boots; host tests still
  pass): counter running, tasks/heap/ls/cat/malloc all produce
  clean output lines while the counter counts; touch/write/ls/
  cat hello.txt (15 bytes) work concurrently with the counter
  with no corruption, and heap accounting stays exact (used
  4192 -> 4368 = malloc 64 block plus file struct and data);
  spawn busy, sleep 2 with busy and counter running completes
  on time with the counter counting straight through the sleep
  (proving the sleep lock release works, not just that sleep
  returns); tasks shows shell/counter/busy cleanly; kill busy,
  kill counter, tasks shows shell only, no further counter
  output. Typed echo interleaving still visible and expected.

Next: the program layer: user mode, syscalls, and programs
that run on RingTail instead of inside its kernel. Disk
persistence and a GUI remain further out, after that.

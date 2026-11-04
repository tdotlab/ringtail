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
make ringtail.bin CC=gcc 'LD=ld -m elf_i386' CFLAGS='-ffreestanding -O2 -Wall -Wextra -nostdlib -lgcc -fno-builtin -fno-stack-protector -m32'
qemu-system-i386 -kernel ringtail.bin -m 16 -display none -monitor unix:/tmp/ringtail-mon,server,nowait

v0.3.2 - Layout (no behavior change)
- Sources moved from a flat ringtail/ into src/ by subsystem:
  src/boot (boot.asm, linker.ld), src/kernel (kernel.c, idt.c,
  isr.asm), src/drivers (keyboard.c, timer.c), src/mm (heap.c),
  src/fs (ramfs.c), src/shell (shell.c). tests/ unchanged.
  Makefile object list and linker path updated, tests Makefile
  paths updated. No source file includes another, so the move is
  mechanical; rebuild, host tests, and QEMU boot re-verified
  after the move.
- Build artifacts (.o, ringtail.bin, ringtail.iso, isodir) are not part
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

v0.6.0 - Program layer: user mode, syscalls, first real programs
- What changed, in one line: RingTail now has programs, not just
  tasks. A task was a function the kernel ran; a program is code
  that runs ON the kernel, in ring 3, and asks the kernel for
  things instead of calling it. The shell command is `run
  <hello|count|crash>`.
- kernel.c: a kernel GDT that replaces boot's, adding user code
  (0x1B) and user data (0x23) segments at DPL 3, plus a TSS whose
  esp0/ss0 the CPU uses to switch to a kernel stack on every ring
  3 trap. Installed before the IDT, while interrupts are off.
- task.c/task.asm: user tasks get two stacks (kernel stack for
  switch_to and trap frames, user stack for the program itself)
  and a user_entry stub that loads user data segments, builds an
  iret frame (user SS/ESP, EFLAGS with interrupts on, user CS,
  entry address), and irets into ring 3. TSS esp0 is set at
  creation and again after every switch, before the task returns
  to ring 3, so a trap from user task B never lands on task A's
  kernel stack. Cleanup frees the user stack too.
- isr.asm/idt.c: syscall gate, int 0x80, IDT flags 0xEE (DPL 3, so
  ring 3 code may invoke it; a DPL 0 gate would fault). The
  handler saves registers, re-enables interrupts (a print syscall
  spins on the print lock, and with interrupts off the holder
  could never run to release it), calls syscall_dispatch, and
  returns the result in eax via the saved-eax slot.
- syscall.c: three syscalls, eax = number, ebx/ecx/edx = args,
  eax = result. Print (pointer + length, null/length checked,
  under the print lock), sleep (task_sleep), exit (task_exit,
  never returns). The kernel provides primitives; formatting and
  string building are program work.
- programs.c: hello (prints, exits), count (formats its own
  numbers, prints 0-4 with a sleep between, prints done, exits),
  crash (executes cli on purpose, to prove ring 3 is enforced).
  Programs never call kernel functions directly; they trap
  through int 0x80 only. Their bytes live in the kernel image in
  this slice, which is packaging, not privilege: the CPU enforces
  the ring, not the file location.
- Honest limits, stated plainly: ring 3 stops privileged
  instructions and port IO, it does NOT stop memory accesses,
  because there is no paging yet and no per-program address
  space. A program could still read or write kernel memory if it
  tried. The memory wall is a paging-era layer, not this one.
  Syscall pointer validation is therefore shallow (null and
  length only). Also, a faulting program halts the whole OS: the
  exception handler prints and halts, there is no per-program
  fault recovery yet.
- Test (QEMU, -kernel and GRUB ISO boots; host tests still pass):
  run hello prints "Hello from user mode!" and exits, tasks shows
  shell only; run count prints 0-4 with sleeps while the shell
  stays responsive, then "count done", tasks shows shell only;
  kernel counter and user count run side by side with clean
  output lines, count finishes, counter continues, kill stops
  it, tasks shows shell only; run crash in its own boot gives
  "EXCEPTION 13 - system halted", proving a ring 3 cli faults.
  One build bug, caught by the linker: boot.asm's stack_top was
  not global, so task_init's extern failed to link; adding
  `global stack_top` fixed it.

v0.7.0 - Disk, paging, loader: one combined piece, not three demos
- What changed, in one line: programs now live on disk, load into
  their own memory, and run there. Loading from files, paging,
  and disk persistence were built as one piece, on T's call,
  because separately each would have a hole: loading from
  RAM-only files is not really files, and paging with nothing to
  load into it is not really programs. The shell command is now
  `run <name>`, loading from disk, and the shell's file commands
  (ls, touch, cat, write, rm) now operate on disk, not RAM.
- paging.c (new, src/mm): classic two-level x86 paging, no PAE.
  The kernel identity-maps the first 16 MB (virtual = physical),
  supervisor-only, which covers the kernel image, the heap, VGA,
  and every frame the allocator hands out, because the frame pool
  is capped at 16 MB. A frame is a physical 4 KB block; the free
  list is threaded through the frames themselves (first word =
  next free frame), so no extra memory tracks them. Each user
  task gets its own page directory (kernel identity entries
  copied in, user range empty until the loader fills it), and
  switch_to loads CR3 from the new task's directory, flushing
  the TLB. User pages are at 0x08000000 and up, one page table
  per task (directory index 32), present + read/write + user
  (0x07); kernel pages are 0x03 (supervisor), so a ring 3 access
  to kernel memory faults instead of succeeding. That is the
  memory wall v0.6.0 said was a paging-era layer: it is here now.
- task.c/task.asm: task_t gains pd (physical page directory) and
  user_stack_top becomes a VIRTUAL address, not a heap pointer:
  a user task's stack lives in its own address space, because
  heap pages are supervisor-only now and a ring 3 stack there
  would fault on first use. user_entry reads entry (offset 32)
  and user_stack_top (offset 16) as virtual addresses, with no
  add-4096 anymore: the loader computes the stack top from the
  program size plus a guard page. task_cleanup destroys the user
  page directory, which frees its page table and every program
  and stack frame it maps; kernel tasks share the kernel
  directory, which destroy refuses to free.
- isr.asm/idt.c: vector 14 gets its own handler, not the generic
  exception stub, because a page fault carries CR2 (faulting
  address) and an error code, and those are the diagnostics that
  tell a paging bug from any other fault. The handler prints
  both and halts; per-task fault recovery (kill the program, keep
  the OS running) is NOT built yet, so a faulting program still
  halts the OS, just with a much better diagnostic.
- ata.c (new, src/drivers): ATA polling PIO, primary master only,
  LBA sectors, 512 bytes at a time, 256 words per sector through
  port 0x1F0, commands 0x20 (read), 0x30 (write), 0xE7 (flush).
  Polling, not interrupts, for a first driver: simpler, and a
  finite timeout turns a missing disk into an error instead of a
  hang, so a boot without a disk attached still works, with file
  commands reporting "no disk".
- diskfs.c (new, src/fs, replaces ramfs.c): a tiny flat
  filesystem, chosen over FAT for this piece because FAT's
  chains, cluster allocation, and 8.3 names would have doubled
  the filesystem code and crowded out the paging and loader work
  it shares the version with. FAT is a later contained swap, not
  ruled out: the ATA driver and the loader do not change, only
  the directory logic does. Layout, in 512-byte sectors: sector
  0 superblock (magic "RTFS", version, totals, region starts),
  a fixed 64-entry directory in sectors 1-6 (name up to 31 chars,
  start sector, size, used flag, 48 bytes each), an 8-sector
  bitmap in sectors 7-14 (one bit per data sector), data from
  sector 15, each file one contiguous run. The directory and
  bitmap are cached in RAM at init and written back on every
  mutation; data sectors are read/written directly on each
  operation. Writes allocate before freeing the old run, so a
  failed write leaves the old file intact. No journaling: a crash
  in exactly the wrong moment can still leave bitmap and
  directory disagreeing, and a file that cannot fit in one
  contiguous run fails instead of fragmenting. Those are toy
  limits, stated, not hidden. Shell return codes: -4 means no
  disk, and the shell says so instead of "no such file".
- loader.c (new, src/kernel): a flat-binary loader, the smallest
  honest one. A program file is raw machine code linked at
  0x08000000 with entry at offset 0 (no ELF parser, no
  relocation: the file says where it runs, the loader puts it
  there, and because every program loads at the same virtual
  address in its own address space, two programs can both live
  there at once on different frames). Steps: read the file whole
  (capped at 64 KB), create a page directory, map one frame per
  program page and copy bytes through the frame's identity-mapped
  address, leave one guard page unmapped, map one stack page,
  hand directory + entry + stack top to task_create_user. Any
  failure frees what was allocated and returns -1, with no
  half-built task left behind.
- src/programs (replaces programs.c): hello, count, and fault as
  separate freestanding programs, linked by programs.ld at
  0x08000000 with _start first (section .text._start, verified
  by nm: all three at 08000000), sharing only syscall_stub.h
  (int 0x80, PRINT 1, EXIT 2, SLEEP 3, same numbers as the
  kernel). hello prints "Hello from disk!" (a new message, so
  the test proves it came from disk, not from the kernel image);
  count formats its own numbers and prints 0-4 with sleeps;
  fault reads address 0 on purpose, replacing v0.6.0's crash
  (cli) with a memory fault: a present supervisor page read from
  ring 3. tools/mkdisk.py (new, host tool, not OS code) packs
  program binaries and text files into a 16 MB RTFS image in
  argument order; the image itself is not checked in.
- Test (QEMU -kernel and GRUB ISO, both with a -drive image;
  host tests 66 PASS): boot prints "Disk: RTFS, 4 files" with
  paging on and kernel tasks working; ls shows hello (160),
  count (340), fault (52), welcome (30); cat welcome reads its
  data sectors; run hello prints "Hello from disk!" and exits;
  kernel counter and disk-loaded count run side by side with
  clean lines, count finishes, counter continues; write
  notes.txt in one boot, reboot with the same image, ls shows
  5 files and cat notes.txt shows the content (persistence
  proven, not claimed); run fault in its own boot gives "PAGE
  FAULT addr=0x00000000 code=5 - system halted" (present page,
  read, user mode: the memory wall doing its job); boot with no
  disk attached prints "Disk: not found, files unavailable" and
  the shell still works, with ls saying "no disk" and run hello
  failing cleanly. Host tests (tests/, make run): heap tests
  unchanged, RAMFS tests replaced by diskfs tests against a mock
  512-sector disk (create/duplicate/long name/write/read/
  rewrite/auto-create/empty/missing/max 0/delete, multi-sector
  1200-byte file, delete-and-reuse, re-init persistence, and a
  disk-full write that must fail while old files survive), 66
  checks, all passing. One test-driver bug, caught in the run:
  the first HMP script waited out a 5-second socket timeout per
  keystroke; reading to the (qemu) prompt instead cut a 7-command
  run from over two minutes to under thirty seconds.
- Honest limits, stated plainly: paging is real (per-program
  address spaces, supervisor kernel pages), but fault recovery
  is not yet built, so a faulting program still halts the OS;
  there is no NX bit in classic paging, so present user pages
  are executable; syscall pointer validation is still shallow
  (null/length only, and program buffers are now user pages,
  which the kernel reads through the task's own directory);
  disk writes have no journal, so a crash mid-write-back can
  leave the filesystem inconsistent; programs are capped at
  64 KB and one contiguous run per file; only the primary master
  ATA disk is supported, no DMA, no second disk, no ATAPI.
v0.7.1 - Fault recovery: a faulting program dies, the OS keeps running
- What changed, in one line: page faults and generic exceptions
  in a user program now kill that program instead of halting the
  whole OS. The v0.7.0 diagnostic ("PAGE FAULT addr=... code=...")
  is still printed, but the ending changed from "- system
  halted" to "- task killed" for user tasks, and the shell
  prompt comes back.
- task.c: a new helper, task_current_is_user, which is true
  exactly when the current task has a virtual user stack top
  (kernel tasks have 0 there). No new scheduler code was needed:
  fault recovery reuses task_exit, which marks the task DEAD and
  yields away, never returning, and the main loop's cleanup
  frees its kernel stack, page directory, page table, and every
  program and stack frame the directory maps. If no other task
  is ready at fault time (the shell is sleeping, say), the exit
  loop spins yielding until a timer tick wakes one, which is the
  same behavior a normal exit already had, so no new scheduler
  edge case was introduced.
- paging.c (page faults) and idt.c (generic exceptions): both
  handlers break the print lock first, print the diagnostic,
  then check task_current_is_user. User task: print "- task
  killed" and call task_exit (never returns, so the generic
  handler's caller, exception_common with its popa and hang,
  never runs for a user fault). Kernel task: print "- system
  halted" and halt, as before, because a kernel fault is an OS
  bug, and killing the shell would leave no prompt at all.
  Interrupts are off in the trap, so the diagnostic print cannot
  interleave with another task's print, and breaking the lock
  first is what keeps later prints from deadlocking on a dead
  owner: a fault can happen while the program holds the lock
  through a syscall, and that owner will never run again to
  release it.
- The decision is by TASK, not by privilege level at fault time,
  and that distinction matters. A syscall with a bad user pointer
  faults in ring 0, while the kernel reads the pointer, but the
  blame belongs to the program that supplied it, so it is killed,
  not halted. Deciding by privilege level would have halted the
  OS for a program's bad pointer, which would be exactly wrong.
  This also changes the meaning of the shallow syscall pointer
  validation: a bad pointer used to be an OS-killer, now it is a
  program-killer, which is the right tradeoff for this scope
  (checking every page a pointer spans would be the full
  validation, and recovery is simpler and more robust).
- src/programs: fault (null dereference, user-mode page fault)
  stays, and two new disk programs cover the other fault paths.
  crash executes cli in ring 3, a general protection fault
  (vector 13), the v0.6.0 crash program brought back as a disk
  file. badptr asks the kernel to print from an unmapped address
  (0x09000000), which passes the null/length check and faults
  during the print syscall, in ring 0, with error code 0
  (not-present, read, kernel mode). Both are killed, not halted.
- Test (QEMU -kernel and GRUB ISO, both with a -drive image;
  host tests still 66 PASS): run fault gives "PAGE FAULT
  addr=0x00000000 code=5 - task killed", prompt returns, tasks
  shows shell only, run hello after it works; run crash gives
  "EXCEPTION 13 - task killed", same survival; run badptr gives
  "PAGE FAULT addr=0x09000000 code=0 - task killed", same
  survival, proving the task-based decision; a counter and a
  count running side by side survive a fault in a third program
  (fault killed, count finishes with "count done", counter keeps
  counting, tasks shows shell + counter, kill counter, tasks
  shows shell only). One build note: the kernel build is clean,
  no warnings, and all five programs still link at 0x08000000.
- Honest limits, updated: user faults are now survivable, kernel
  faults still halt (by design, not by omission); there is still
  no NX bit, still no journaling, still a 64 KB program cap, and
  a fault during a disk write (which only the shell does, as a
  kernel task) would still be a kernel fault and halt, because
  programs do not yet have file syscalls to fault through.
v0.7.2 - File syscalls: programs can open, read, write, close, and delete files
- What changed, in one line: a program can now use the disk,
  not just the screen. Five new syscalls (OPEN 4, READ 5, WRITE
  6, CLOSE 7, DELETE 8, alongside PRINT 1, EXIT 2, SLEEP 3), all
  through the same int 0x80 doorway, with the numbers matching
  exactly between syscall.c and syscall_stub.h.
- task.c: per-task file descriptors, 8 slots (0-7) per task, held
  in the task struct itself (fd_used, fd_offsets, fd_names), at
  the END of the struct on purpose, so the offsets task.asm
  relies on (esp 0, pd 12, user_stack_top 16, entry 32) do not
  shift. Descriptors are per-task, not global: two programs can
  both hold fd 0 for different files. The helpers
  (task_fd_open/close/lookup/advance) live in task.c because
  only it sees the task_t definition; syscall.c calls them
  through externs and never touches the arrays directly.
- syscall.c: open takes a filename and returns a slot, creating
  the file if it does not exist (like the shell's write
  auto-create) and resetting the offset to 0; read and write
  take a slot, a user buffer, and a length capped at 4096, and
  return bytes transferred or -1; read returns 0 at end of file;
  close frees a slot; delete takes a filename and does not close
  descriptors already open for it (later operations on those
  descriptors fail, because the file is gone when re-read by
  name). There are no modes and no permissions in this slice:
  every open is read/write, every program can open any file,
  because there are no users yet to give permissions to. There
  is no seek yet: offsets advance only through read and write,
  and reopening resets to 0, so appending means reading the old
  content first. Errors are all -1, with no errno, which is the
  toy tradeoff for not inventing an error-number table yet.
  Read and write work by reading the whole file into a heap
  buffer (diskfs_read_all, capped at 64 KB like the loader),
  slicing or rebuilding it at the offset, and writing it back,
  so a write at an offset past the end zero-fills the gap, and
  a failed write leaves the old file intact (diskfs
  allocate-before-free). Names are copied from user space with
  a 32-byte bound, so the kernel never reads an unbounded user
  string, and an unmapped user pointer faults and kills only the
  caller (v0.7.1 recovery), which is what makes the bounded
  validation survivable rather than OS-killing.
- diskfs.c: the string write is now a thin wrapper around a new
  length-based write (diskfs_write_bytes), because program bytes
  are not strings: a program file may contain null bytes, and
  strlen would truncate it at the first one. The shell's behavior
  is unchanged (it passes text, the wrapper passes its length),
  and the host diskfs tests still pass against the same logic.
- src/programs: filedemo, the first program that uses the disk.
  It opens demo.txt, writes "Hello from a program!", closes,
  reopens, reads the line back, and prints it, then checks two
  error paths (reading a closed descriptor must fail, a
  32-character name must fail) and prints whether each behaved
  as expected. It deliberately leaves demo.txt on disk, so the
  shell can cat it afterward: program and shell sharing one
  filesystem, proved, not claimed.
- Test (QEMU -kernel and GRUB ISO, both with a -drive image;
  host tests still 66 PASS): run filedemo prints "fd=0", "wrote
  22 bytes", "read back: Hello from a program!", "closed fd
  read correctly failed", "long name correctly rejected"; cat
  demo.txt from the shell shows the program's line; ls shows
  demo.txt (22 bytes) alongside the programs; running filedemo
  a second time prints the identical output (overwrite at offset
  0, not append, so no duplication); rm demo.txt from the shell
  deletes the program's file, and ls confirms it is gone; run
  hello and run fault afterward still work, with fault still
  killing only itself and tasks showing shell only, so the
  syscall changes did not regress fault recovery.
- Honest limits, updated: programs can now read and write files,
  but not list the directory (no LIST syscall yet, the shell
  still owns listing, and a LIST format deserves its own design,
  not a rushed one); no seek, no append mode, no file locking,
  so concurrent writes to the same file from two programs are
  not atomic (each write reads, modifies, and writes back, and
  another task can interleave between read and write, so the
  last writer can win with stale content: programs should not
  share a file concurrently yet); files over 64 KB cannot be
  read or written through syscalls in this slice (same cap as
  the loader); errors are all -1, with no reason given.

v0.8.0 - Mouse + graphics: PS/2 mouse, VGA mode 13h, drawing primitives
- What changed, in one line: RingTail can now see a mouse and
  draw pixels. The shell gains two commands: `mouse` (position,
  buttons, packet count) and `gfx` (a graphics demo that draws
  shapes and text, shows a cursor that follows the mouse, draws
  a dot where you click, and returns to the text shell on any
  keypress, with the shell screen exactly as it was).
- mouse.c (new, src/drivers): PS/2 mouse on the keyboard
  controller, IRQ 12 (slave IRQ 4, IDT vector 44, slave PIC
  unmasked for that line only, master mask unchanged). The mouse
  does not report a position, it reports movement in three-byte
  packets (buttons, X delta, Y delta, with sign and overflow
  bits), so the driver accumulates a cursor position and clamps
  it to 0-319, 0-199. Y is inverted (PS/2 positive means up,
  screen positive means down), overflow on an axis means that
  axis is ignored for that packet, and a first byte without bit
  3 set is dropped as a resync, because a lost byte would
  otherwise shift every later packet. The handler reads, updates,
  and sends EOI to both PICs (slave first); it never prints, so
  it can never deadlock on the print lock. Init, with finite
  timeouts at every step so a missing mouse cannot hang the
  boot: drain stale output first (power-on self-test bytes left
  in the controller will otherwise be read as the command byte
  and written back as garbage, which is exactly what the first
  v0.8.0 QEMU run showed, with typing dead and no mouse),
  enable the aux device, read the controller command byte, set
  mouse IRQ on and mouse disable off, write it back, reset the
  mouse (ACK 0xFA first, then self-test 0xAA, then ID 0x00: a
  reset ACKs before it reports, and waiting for 0xAA straight
  after 0xFF reads the ACK and fails, which is what the debug
  run showed), set defaults (0xF6, ACK), enable reporting (0xF4,
  ACK). A failed init drains the controller and reports "not
  found" instead of hanging, and the graphics demo still runs,
  just with a cursor that cannot move.
- isr.asm/idt.c: mouse_isr stub (pusha, call, popa, iret, same
  shape as keyboard_isr), IDT gate 44, slave PIC mask 0xFF to
  0xEF (only IRQ 12 unmasked on the slave). Without the gate, the
  first mouse movement after sti would triple-fault.
- vga_gfx.c (new, src/drivers): mode 13h (320x200, 256 colors,
  one byte per pixel at 0xA0000, already identity-mapped, so no
  paging change was needed) set by writing VGA registers
  directly, because BIOS calls do not work in protected mode:
  miscellaneous, sequencer (with the synchronous-reset dance),
  CRTC (unprotecting register 0x11 first), graphics controller,
  attribute controller (flip-flop reset before every register,
  palette re-enabled at the end or the screen stays blank), and
  a 16-color demo palette through the DAC. Primitives, all
  through one bounds-checked pixel so off-screen drawing clips
  instead of writing past the 64K framebuffer: clear, line
  (Bresenham), rectangle (filled and outline), circle (midpoint,
  eight-way symmetry), and text. Text uses the real VGA font,
  read out of video memory plane 2 where the BIOS loaded it,
  not a hand-drawn approximation: glyph n lives at font[n*32 +
  row], because the character generator addresses each glyph in
  a 32-byte slot (an 8x16 font uses the first 16 bytes, the rest
  is padding), and a 16-byte stride maps every character to the
  wrong glyph, which is what the first screenshots showed.
- Save and restore, the reason the demo is safe to run: before
  switching, the driver saves the VGA register state, the full
  256-color palette, the 4000-byte text screen, and the font;
  after the demo it restores all four, so the shell comes back
  exactly. The font needs restoring because mode 13h writes go
  through the same video memory the font lives in (plane 2), so
  the demo would otherwise destroy the text font. Reading or
  writing a single plane needs the graphics mode register's
  odd/even addressing off (text mode has it on, and it overrides
  plane selection with the address bit) and the memory map
  pointed at 0xA0000, both restored afterwards. One bug, caught
  by the post-demo garbage screen: the sequencer map mask is a
  bitmask, not a plane number, so plane 2 alone is 0x04, not
  0x02; writing 0x02 selected plane 1, overwrote the text
  attributes with font bytes, and left the font broken. The
  graphics read map IS a plane number, so font loading (a read)
  was correct all along, which is why demo text worked while
  restore failed.
- gfx_demo, in vga_gfx.c: title, filled red rectangle, yellow
  outline rectangle, green line, red circle, alphabet and digit
  lines (the font proof), instructions, and a live coordinates
  line (X, Y, L/M/R buttons, fixed width, redrawn on any mouse
  change). The cursor is a 12x16 arrow, tip at the reported
  position, drawn with save-underneath and restore, so it leaves
  no trail and a clicked dot survives the cursor passing over
  it. Left click draws a 4x4 yellow dot at the tip. The loop is
  paced by the timer tick (~100 Hz, hlt between frames), polls
  the keyboard queue itself (the main loop is inside the demo,
  being a shell command, so it drains the queue or the exit key
  would leak into the next shell line), and exits on any key's
  make code, draining the rest of the queue first. The shell
  releases the print lock around the demo, like the sleep
  branch, so other tasks are not stalled for the demo's whole
  duration; the demo draws only to the framebuffer, which no
  other task touches, so it needs no lock itself.
- kernel.c: boot banner v0.8.0, mouse_init called after timer
  init and before task init (polling works with interrupts off;
  packets wait in the controller until sti, when the IRQ 12
  handler is ready), printing "Mouse: PS/2 found" or "Mouse:
  not found" like the disk message. shell.c: `mouse` and `gfx`
  commands, help and version updated, 25 commands total.
- Test (QEMU -kernel and GRUB ISO, both with a -drive image;
  host tests still 66 PASS, unchanged because mouse and graphics
  are port I/O and VGA memory, QEMU-only like paging and the
  timer): boot prints "Mouse: PS/2 found"; `mouse` reports
  x=160 y=100 buttons=0 packets=0 (centered, no movement yet);
  `gfx` shows the demo with title, shapes, alphabet, digits,
  instructions, coordinates, and cursor at center; a mouse move
  moves the cursor and updates the coordinates with no trail
  left behind; a left click shows L in the coordinates and draws
  a dot that is still there after the cursor moves away; Esc
  returns to the text shell with the exact pre-demo screen,
  colors, and font, and `version` afterward prints cleanly.
  Three bugs were found and fixed in testing, all in the new
  code, none in earlier versions: the stale-output drain (typing
  dead on first boot), the reset ACK order (mouse not found),
  and the font stride plus the map-mask bitmask (garbage text
  after the demo, fixed by the 32-byte stride and 0x04 mask).
- Honest limits, updated: graphics lives in the kernel in this
  slice, not behind syscalls, so a user program cannot draw yet
  (that is the next graphics step, when a GUI program needs it);
  the demo is the only graphics mode user, and the shell stays
  text; the mouse has no acceleration or scaling (raw deltas,
  one pixel per unit, which is right for 320x200); there is no
  mouse wheel support (the three-byte packet has no wheel byte,
  a wheel needs the four-byte Intellimouse protocol, a later
  addition); a missing mouse is handled (not found, demo still
  runs) but was not QEMU-tested, because QEMU always presents a
  PS/2 mouse, and the timeout path is code-reviewed, not
  run-verified.

v0.9.0 - GUI: a graphical desktop as a user program, plus the
graphics and input syscalls it needs
- What changed, in one line: `run gui` opens a desktop with a
top bar, two icons, and two draggable windows with buttons,
built entirely by a program in ring 3 through new syscalls, not
by kernel demo code, and Quit (or Esc) returns to the text shell
exactly as it was.
- Syscalls 9-17 (numbers must match syscall_stub.h exactly):
GFX_BEGIN 9 (enter graphics mode, save the text screen, become
the graphics owner), GFX_END 10 (restore, release ownership),
GFX_PIXEL 11 (x, y, color in registers), GFX_CLEAR 12 (color),
GFX_RECT 13 (pointer to {x, y, w, h, color, filled}),
GFX_LINE 14 (pointer to {x0, y0, x1, y1, color}),
GFX_TEXT 15 (pointer to {x, y, string, color}, string copied
to a kernel buffer with a 256-byte bound, longer strings
truncated), MOUSE_STATE 16 (pointer to an output {x, y,
buttons} struct), KEY_GET 17 (next scancode, or -1 if empty).
Multi-argument calls take a struct pointer because the doorway
only carries three register arguments and a rectangle needs
six values; the structs are all ints in kernel-read order, so
there is no padding to disagree about.
- Ownership, the rule that keeps one framebuffer sane: only one
program may own graphics mode at a time, because there is one
saved text screen. BEGIN by the owner again is a no-op success
(it must NOT re-save, or the saved text would be overwritten
with the graphics state it is supposed to restore to); BEGIN
by anyone else fails; drawing calls and KEY_GET fail for
non-owners, so a text program cannot draw over the desktop or
steal the GUI's keys. MOUSE_STATE is read-only and is allowed
for any task, like the shell's `mouse` command. If the owner
dies without calling END (it faults, or forgets), task_cleanup
calls gfx_task_exited, which restores text mode, so the OS
never stays stuck in graphics mode with no program drawing.
- Keyboard routing, the other half of ownership: while graphics
is owned, the kernel main loop leaves the keyboard queue alone
instead of draining it into the shell, and the owner drains it
through KEY_GET, which only the owner may call, so there is
exactly one drainer and no race on the queue tail. Keys typed
during the GUI stay queued for the GUI; the shell never sees
them. The GUI drains the whole queue each frame, so nothing is
left for the shell when it exits, except keys pressed after it
died, which arrive after cleanup clears the owner, in the right
order. A program that calls KEY_GET without owning graphics
gets -1 and touches nothing.
- The mode switch itself was refactored, not duplicated:
vga_gfx.c now has gfx_graphics_begin/end (save text, load the
font while still in text mode, switch to mode 13h, set the demo
palette, clear; and the reverse), used by both the kernel `gfx`
demo and the new syscalls, so the demo and the syscalls cannot
drift apart. One bug that refactor exposed and fixed: the text
cursor lives in two static variables in kernel.c, not in a VGA
register, so the graphics save (video memory plus registers)
did not capture it, and a text print during graphics mode (a
fault diagnostic, or a program printing while it owns the
display) advanced it while the restore reverted the screen,
leaving the next typed command on a fresh line with no prompt.
Begin now saves the cursor through text_cursor_get, end
restores it through text_cursor_set, so cursor and restored
screen always agree, and invisible prints are discarded along
with the invisible screen they wrote to.
- gui.c (new, src/programs, 3940 bytes on disk): the desktop.
Top bar (title, live mouse coordinates, red Quit button),
two icons (Welcome, yellow; About, cyan), two windows
(Welcome: two lines, a "Click me" button, a click counter;
About: version and build lines), each with a title bar, a red
X close button, and a light-gray body. Clicking an icon opens
its window; clicking a window brings it to the front (z-order
is a two-element swap, because there are two windows);
dragging a title bar moves the window, clamped so it stays
fully on screen below the top bar; clicking X closes it;
clicking the Welcome button increments its counter; Quit or
Esc ends graphics mode and exits, returning to the shell.
There is no damage tracking: any change redraws the whole
screen (desktop, bar, icons, windows back to front, cursor
last), which is simple, correct, and fast enough at 320x200,
because a full redraw is a few hundred syscalls and only
happens when something actually changed. The loop polls mouse
and keys, because there is no blocking input wait yet, so the
GUI spins in its time slices; preemption keeps everything else
responsive, but it is wasteful, a known limit. The cursor is
the kernel demo's arrow shape, drawn with a black shadow
offset by (1,1) under the white arrow, so it stays visible on
both the blue desktop and the light-gray window bodies.
- One real bug, found by the first GUI screenshots and fixed
before anything else was trusted: the desktop appeared as a
bare blue screen with only the cursor, no bar, no icons, no
windows, and mouse clicks did nothing, because pixels (plain
numbers in registers) drew and rectangles and text (struct
pointers) did not. The cause was the program-side syscall
wrapper: one inline asm statement, with no "memory" clobber,
so the compiler was allowed to keep a program's struct-field
writes in registers, or reorder them after the interrupt,
because it could not see that the interrupt reads that memory
through the pointer. The kernel then read unwritten stack and
drew nothing (or, for text, read a null string pointer and
returned -1), while the program stayed alive, which is why the
screen stayed blue instead of faulting back to text. The fix
is the "memory" clobber on do_syscall in syscall_stub.h, which
forces every pending write out before the interrupt and forces
a reload of anything the kernel wrote, after it. It applies to
every program that includes the header, which is why count grew
from 340 to 372 bytes and gui from 1044 to 3940: the compiler
can no longer delete struct writes it used to call dead.
- Test (QEMU -kernel, GRUB ISO, and a scratch fault program
that enters graphics and then faults on purpose; host tests
still 66 PASS, unchanged, because the new syscalls are
QEMU-only like the graphics they drive): boot prints "Disk:
RTFS, 8 files"; `run gui` shows the desktop with title,
coordinates X=160 Y=100, Quit, both icons, and cursor at
center; clicking the Welcome icon opens its window at
(80,50,180,120) with button and "Clicked: 0"; dragging its
title bar by (40,30) moves it to (120,80) with no ghost of the
old position; clicking "Click me" shows "Clicked: 1"; clicking
Quit returns to the text shell with the exact pre-GUI screen,
and `version` afterward prints cleanly, proving keyboard
routing is restored; GRUB ISO shows the same desktop and Esc
quits to a working shell; the fault program returns to text
instead of staying stuck in blue graphics mode, and `version`
after it prints as "ringtail> version" on one line, proving
the cursor fix. Pixel-level checks on the screenshots
confirmed the window edges, close button, body, and dragged
position, not just the eyeball view.
- Honest limits, updated: the desktop cannot launch programs
yet (no LIST or SPAWN syscall, so icons only open built-in
windows); a faulted graphics program's diagnostic is lost,
because the text restore overwrites the fault print that
happened during graphics mode (the safety property, not
getting stuck, holds, the message does not, and a fault log is
a later piece); the GUI polls instead of waiting for input;
there is no text input in windows, no clock in the top bar
(no tick syscall yet), and no window resizing or minimizing,
because two fixed windows do not need them yet. The kernel
`gfx` demo still works unchanged through the refactored
begin/end, verified after the refactor.

v0.10.0 - FAT filesystem: RTFS replaced by standard FAT16,
as a contained swap, with the same diskfs API throughout
- What changed, in one line: the disk is now FAT16, the
standard filing system used by DOS, Windows, USB sticks, and
SD cards, instead of the homemade RTFS flat layout, so a
RingTail disk is readable by real tools and a disk made by a
real tool is readable by RingTail, with no change to the ATA
driver, the loader, the syscalls, or the shell's file commands
beyond the version strings and one error message.
- Layout: superfloppy (no partition table, FAT starts at
sector 0, as RTFS did), 512-byte sectors, 1 sector per cluster
in our own images, 1 reserved sector (the boot sector with
the BPB), two identical FAT copies, a 512-entry root
directory, then data. Our 16 MB image works out to FATs of
127 sectors each, root 32 sectors, data from sector 287,
32481 clusters (2..32482), which is in the FAT16 range. The
kernel does not hardcode any of that: it parses the BPB at
init (bytes per sector, sectors per cluster, reserved,
FAT count, root entries, total sectors, FAT size, boot
signature) and derives FAT start, root start, data start,
and cluster count from it, and it accepts any power-of-two
cluster size, so a disk made by mkfs.fat works too. The
cluster count decides the type, per the spec: below 4085
is FAT12, at or above 65525 is FAT32, and both are rejected
with "not a FAT16 filesystem" instead of being misread.
- Chains, the core difference from RTFS: a file is a chain
followed through the FAT (0 = free, a cluster number = next
piece, 0xFFF8 and up = end), so its pieces need not sit
together. Deleting a file in the middle leaves holes, and a
new file fills them first-fit, which RTFS could not do (it
needed one unbroken run, and failed when none fit, even with
free space scattered around). Writes keep the RTFS
allocate-before-free shape: allocate a new chain, write its
sectors, then free the old chain, then write back the FAT
copies and the root directory, so a failed write leaves the
old file intact on disk, because the FAT and directory have
not been flushed yet when the failure is detected. The FAT
and root directory live in RAM after init (65 KB + 16 KB
for our image) and are written back on every mutation;
data sectors are never cached. All public functions run
under interrupt save/restore, as before.
- Names are DOS 8.3, the visible change: at most 8
characters, a dot, at most 3 for the extension, stored
uppercase, space-padded, with no dot stored. The kernel
converts user names to that form on every operation, so
typing lowercase still finds the file, and converts back
for listing, so ls now shows HELLO, DEMO.TXT, and so on,
instead of the lowercase names RTFS preserved. Names that
do not fit 8.3, or use a forbidden character, are rejected
with -2, and the shell's touch message now says "Name must
fit 8.3 (e.g. DEMO.TXT)" instead of "max 31". The syscall
name buffer (32 bytes) and the per-task fd names did not
change: 8.3 names fit easily, and fd lookups work because
every diskfs call normalizes the same way.
- Volume labels: mkdisk.py now writes a volume label entry
("RINGTAIL", attribute 0x08) as root entry 0, matching the
BPB label, so fsck does not report a label mismatch. The
kernel skips volume entries (and long-name entries, and
directories) when listing, finding, counting, and checking
collisions, so the boot banner still counts regular files
only (8, not 9, for the standard image).
- mkdisk.py: same command line as before (name:file pairs,
-o, --size-mb), now building FAT16 instead of RTFS, with
the same BPB fields the kernel parses, clusters allocated
in argument order (so an initial image's chains happen to
be contiguous), and a volume label entry first. The disk
image is still not checked in; rebuild it whenever the
programs change. Old RTFS images are unreadable now, by
design: the format changed, so any old disk.img must be
rebuilt with the new tool.
- Test: host tests are 84 checks, all passing (was 66). The
mock disk grew to 8192 sectors (4 MB) because a smaller
mock would have fewer than 4085 clusters and would be
FAT12, which the kernel correctly rejects, so the mock had
to be big enough to be FAT16. New checks cover 8.3 edges
(too-long base, multiple dots, slash, lowercase lookup),
a fragmented chain (A=2,3,4; B=5,6,7; C=8,9,10; delete B;
write a 5-cluster file, and its chain is verified from the
mock FAT to be 5,6,7,11,12, jumping over C), a disk-full
write one byte over the remaining free space (fails, old
file still readable), and a FAT12 mock (init refused,
create returns -4). QEMU, all passing: boot prints
"RingTail v0.10.0 - FAT filesystem" and "Disk: FAT16, 8
files"; ls, cat (lowercase and uppercase input), touch,
write, rm, and the 8.3 error messages all behave as above;
run hello, run count, run filedemo (twice, idempotent),
run fault/crash/badptr (task killed, shell survives), and
run gui (desktop identical to v0.9.0, proving an 8-cluster
program loads through a chain); write-then-reboot persists;
GRUB ISO boots with the FAT disk; no-disk boot still says
"Disk: not found" and the shell still works. External
tools, both directions, installed for the test via apt
(dosfstools, mtools; apt tools do not survive VM
replacement): fsck.fat on a RingTail-written disk is clean
(10 files, 19 clusters used, free space matching the
cluster arithmetic exactly); mdir lists the same files;
mcopy reads PERSIST.TXT back byte-for-byte; and a disk
made by mkfs.fat -F 16 -s 1 (whose BPB turns out identical
to ours: same FAT size, same data start, same cluster
count) with files copied in by mcopy boots in RingTail,
lists 2 files (volume skipped), cats, and runs hello.
Kernel size 59,688 bytes (was 59,340), 0 warnings.
- Honest limits, updated: no long file names (a file made
elsewhere with a long name is reached by its short name,
like LONGFI~1.TXT, and deleting it leaves its long-name
entries orphaned, harmless but wasted slots, until a new
file reuses those slots); no subdirectories (root only, as
before); no timestamps (date/time fields are zero, so
external tools show 1980, the FAT epoch); no journaling
(a crash mid-write can leave FAT and directory
disagreeing, as with RTFS); FAT16 only, FAT12 and FAT32
rejected; the 64 KB whole-file cap from read_all stays,
so FAT itself has no such cap but programs still do; the
FAT must fit in the heap (65 KB for our image, fine, but
a huge disk's FAT would fail init with out of memory);
cluster allocation scans from cluster 2 each time, so
writing a very large file is quadratic in clusters, which
is fine for the small files this OS actually writes.

v0.10.1 - Seek: one new syscall, SEEK 18, that moves an open
file's offset, completing the file API from v0.7.2
- What changed, in one line: a program can now jump to a
byte position in an open file, instead of only reading
forward from where it left off, so it can reread a header,
skip ahead, or read the tail without closing and reopening
(which would reset to 0 anyway).
- Syscall 18, numbers must match syscall_stub.h exactly:
SEEK, with a = fd, b = offset (signed 32-bit), c = whence,
returning the new absolute offset, or -1 on failure with
the offset unchanged. Whence follows Unix: SEEK_SET 0
(from the start), SEEK_CUR 1 (from the current offset),
SEEK_END 2 (from the end of the file, so a negative
offset counts back from the end). The implementation in
syscall.c reuses task_fd_lookup and task_fd_advance, so
offsets stay per-task, as before, and it computes the new
offset in 64-bit to detect overflow, then requires the
result to fit in a signed 32-bit int (0..2147483647),
because the program-side wrapper returns int and -1 means
error; a larger offset would be indistinguishable from an
error, and on a 16 MB disk no real offset comes close.
- The one asymmetry, by design: SET and CUR only do offset
arithmetic, so they do not touch the disk and succeed even
if the file was deleted after opening; END needs the
file's size, so it fails if the file is gone or there is
no disk. END uses a new diskfs_size function, not
read_all, because read_all buffers the whole file and caps
at 64 KB: size only reads the directory entry, which is
already in RAM, so it is cheap and works for larger files
too. diskfs_size returns 0 with the size, -1 for a missing
or invalid name, -4 for no disk, under the same interrupt
locking as the other diskfs functions.
- Seeking past end of file is allowed, Unix-style: a later
read returns 0 (end of file, offset unchanged), and a later
write extends the file with a zero-filled gap, by the
existing write logic (which builds old content plus the
new slice, zero-initialized). No new shell command: the
shell's file commands work on whole files, not offsets,
so seek is a program-level operation, and seekdemo is the
proof, not a shell feature.
- seekdemo (2876 bytes): writes "0123456789ABCDEF" (16
bytes, every index its own digit or letter), then checks
SET 5 reading "567", CUR 2 reading "AB", END -4 reading
"CDEF", END 0 reading end of file, SET 100 reading end
of file past the end, CUR -100 back to 0 reading "0123",
and three failures that must leave the offset unchanged,
proved by reading after each one (bad whence 99, SET -1,
CUR -100 from 8), a seek on a closed descriptor, and the
deleted-file asymmetry (SET succeeds, END fails). Each
test prints its seek return and the bytes it read, or an
ERROR line, so the screenshot is the proof, not just a
claim. It leaves seek.txt on disk for the shell to cat,
like filedemo leaves demo.txt. A comment fix rides along:
filedemo's "max is 31" note was stale after FAT (the limit
is 8.3 now), and now says so; the program itself is
unchanged (1304 bytes, same as before).
- Test: host tests are 92 checks, all passing (was 84).
New checks cover diskfs_size on a small file (4), a
multi-cluster file (1200), an empty file (0), a missing
file (-1), a 100,000-byte file (100000, past the read_all
cap, proving size is uncapped), and a FAT12 disk (-4).
Count correction: v0.10.0 was reported as 85 checks, but
that count included the CHECK macro definition line; the
true invocation count was 84, and this version's 92 is
counted the same way (invocations only, plus two printf
PASS lines for diskfs_list, which are not CHECKs). QEMU,
all passing: run seekdemo prints all twelve ok lines,
with SET 100 and CUR -100 on separate lines (an earlier
run had them joined by a one-byte print-length slip, now
fixed, along with three other print lengths that were off
by one); cat seek.txt reads back the sixteen bytes; ls
shows SEEK.TXT; write-then-reboot persists seek.txt;
run filedemo, fault, crash, badptr, count, and gui all
still behave as before (gui desktop identical, Esc
returns to a working shell); GRUB ISO boots and runs
seekdemo identically. Kernel size 59,740 bytes (was
59,688), 0 warnings. A VM replacement mid-build wiped
/tmp and the apt toolchain, so the toolchain was
reinstalled (same versions: QEMU 8.2.2, nasm 2.16.01)
and the seek code was re-applied from the surviving
screenshot and conversation, in a persistent workspace
scratch dir instead of /tmp, then verified from the
canonical tree; the QEMU harness itself was also
reconstructed in ~/workspace/ringtail/harness/ for the
same reason, and it runs a full seek test in about
eleven seconds.
- Honest limits, updated: seek does not change the 64 KB
read_all cap (a program still cannot read a larger file
in one read_all call, though size now reports it, and
the shell's bounded read still reads its first part);
seek cannot create a hole larger than the disk or heap
(a write at a huge offset fails as before, offset
unchanged); there is still no LIST, so a program cannot
discover filenames, and no SPAWN, so the GUI still cannot
launch programs; those remain the launcher piece.

v0.10.2 - GUI launcher. The desktop can now start real
programs by clicking, not just open its own built-in
windows. Two new syscalls, both completing the file
story that seek started. LIST (19) gives a program the
Nth file's name: call it with index 0, 1, 2, and so on
until it fails, and you have every file on disk, in the
same order and with the same names ls prints, because
both go through the same root-directory walk and the
same raw-to-display conversion (uppercase, dot
inserted). The kernel side is a new diskfs_list_at,
which counts regular files only, skipping deleted,
long-name, directory, and volume entries, and entries
with a nonzero first-cluster-high, exactly as ls does.
A buffer that is null, or too small for the name plus
its null terminator, fails with -1 and no partial copy,
never a truncation, and a negative buffer size is
rejected the same way. SPAWN (20) starts a file as a
new task, like the shell's run command, and returns the
new task id, or -1 on failure. It reuses loader_run, so
a spawned program is a normal task, not a special kind.
One guard: SPAWN refuses while the caller owns graphics
mode, because a program started inside graphics mode
would print to a text screen nobody can see, and the
restore on graphics exit would overwrite it, so the
output would be lost, not just delayed. A graphics
caller must end graphics first, then spawn, then exit.
The demo is listdemo (1344 bytes): it checks a
too-small buffer size (passed with a roomy buffer, so
even a kernel that ignored the size could not overflow
the stack, it would only return the wrong value, which
the check catches) and a null buffer, then lists every
file, then spawns hello (which should succeed) and a
missing file (which should fail), printing each result.
The GUI itself, at startup, lists every file, keeps
only names with no dot (programs have no extension,
data files do), and draws one green icon per program,
up to nine, in columns to the right of the built-in
icons (built-in yellow Welcome and cyan About stay
where they were). Clicking a program icon ends
graphics, spawns that program, prints "Started <name>
as task <id>" the way the shell does, and exits, so
the new program runs in text mode, where its output is
visible. Windows are still checked before icons, so a
click on an open window never launches through it, and
clicking the GUI icon itself relaunches the desktop,
which is the graphics-ownership handoff working: the
old desktop ends graphics, the new one begins it.
Two cleanups ride along. The welcome file is now
packed as WELCOME.TXT, not WELCOME, so the
no-extension rule holds: before, welcome was a data
file with no extension, which would have shown up as
a program icon, and cat welcome.txt would not have
found it; now it does, and the launcher correctly
leaves it out. The About window's version was stale
since v0.9.0 and now says v0.10.2, matching the kernel
and shell banners. A layout fix, found by screenshot:
icon rows moved from y=30, 90, 150 to y=26, 86, 146,
so the bottom row's label ends exactly at the screen's
bottom edge (146 + 38 + 16 = 200); at y=30 the bottom
labels were clipped by 4 pixels, proved by pixel counts
in the screenshots, not just by eye.
- A real kernel bug, found by SPAWN and fixed here.
Creating a task set the kernel trap stack (TSS esp0)
to the new task's stack immediately, at creation.
That was fine when the shell created a task, because
the shell never traps, so nothing ran on the new
stack before the new task did. It broke when a program
created a task: the creating program's next system
call trapped onto the new task's stack and overwrote
its startup frame, so the new program jumped to
garbage and faulted. The symptom was exact: a spawned
hello faulted with EXCEPTION 0 and never printed,
while a shell-run hello in the same build printed
fine. The fix is in the right place, not a workaround:
task creation no longer touches esp0 at all, and esp0
is set on first entry, in user_entry, just before the
first iret into ring 3, and on every resume, in
task_yield and task_preempt, before the task returns
to ring 3. Esp0 now always belongs to the task that
is about to run, never to the task just created.
- Test: host tests are 106 checks, all passing (was
92). New checks cover list_at on an empty disk (-1),
on a fresh disk with four files (A.TXT, B.TXT, C.TXT,
PROG, in order, then past the last file, then a
negative index), after deleting the middle file
(A.TXT, C.TXT, PROG, then past the last file,
proving deleted entries are skipped), and on a FAT12
disk (-1), plus the setup creates and delete themselves.
Count is invocations only, plus two printf PASS lines
for diskfs_list, which are not CHECKs. QEMU, all
passing: run listdemo lists all ten files in disk
order, spawns hello (which now prints "Hello from
disk!", after the esp0 fix), and fails the missing
file correctly; ls agrees with LIST on names, order,
and sizes; cat welcome.txt prints "Welcome to
RingTail!"; run seekdemo, filedemo, fault, and gui
all still behave as before; run gui shows nine green
program icons in LIST order (HELLO, COUNT, FAULT,
CRASH, BADPTR, FILEDEMO, GUI, SEEKDEMO, LISTDEMO),
no WELCOME.TXT icon; clicking HELLO exits the desktop
and runs hello in text mode, with "Started HELLO as
task 2" printed; clicking GUI relaunches a fresh
desktop (top bar shows the click position, X=265
Y=43); a minimal disk with only hello, count, gui,
and welcome.txt shows only three program icons,
proving the icons come from LIST, not from hardcoded
names; the Welcome window still opens, and Esc still
exits to a working shell; GRUB ISO boots and runs
listdemo identically (byte-identical screenshot to
the direct-kernel run). Kernel size 59,836 bytes (was
59,740), 0 warnings. GUI grew 3940 to 5024 bytes,
still well under the 64 KB program cap.
- Honest limits, updated: the launcher shows at most
nine programs, and any past nine are simply not shown,
not paged or scrolled; it lists only files with no
extension, so a program with an extension would not
show, and a data file with no extension would show
and fail on launch (old disk images with WELCOME, no
extension, do exactly that: rebuild the disk with the
new welcome.txt packing to fix it); launching a text
program exits the desktop, because text programs print
to the text screen, which graphics mode hides, and a
persistent desktop that keeps text programs visible
would need text rendering in graphics mode, which is
a later piece; the 64 KB read_all cap is unchanged,
and there is still no blocking input wait, so the GUI
still spins polling the mouse and keys.

Next: push (rename + v0.10.0 + v0.10.1 + v0.10.2,
awaiting T's word, do not push unasked); then, as
build candidates, a GUI blocking input wait (so the
desktop stops spinning on poll), a fault log so
graphics-mode fault diagnostics survive the text
restore, and the other open items (64 KB cap
raise/streaming only when a file needs it, heap from
the memory map, VGA text split out of kernel.c, real
headers, UNDERSTANDING.md into a repo docs folder,
mock update past v0.9.0, now three versions behind,
unrequested).

v0.10.3 - Input wait + fault log. Patch, not a new
subsystem: it completes the input story the GUI
launcher started, by giving programs a way to sleep
until input arrives, and by keeping fault diagnostics
where a graphics restore cannot erase them. Built
locally, QEMU-verified, NOT yet pushed (local
canonical only; remote is still v0.10.2, commit
3fe0d806; push only when T says).

WAIT, syscall 21, after SPAWN, mirrored in
syscall_stub.h as sys_wait(last_x, last_y,
last_buttons). The caller passes the mouse state
from its last MOUSE_STATE read. The kernel, with
interrupts off, checks two things: is a key already
queued, and has the mouse moved or changed buttons
since the caller last saw it? If either is true, it
returns at once, without sleeping, so input that
arrived between a poll and the wait is not missed.
Otherwise it marks the task WAIT_INPUT and yields,
and the task stays asleep until a keyboard or mouse
interrupt wakes it. The check and the block are one
atomic step under cli, which is what avoids the
classic lost-wakeup race: an interrupt cannot slip
in between them. WAIT does not consume input: after
it returns, the caller still reads the key with
KEY_GET or the mouse with MOUSE_STATE, so in text
mode a key that wakes a waiter stays queued for the
shell, as the waitdemo key test shows (see below).
Any task may wait, unlike KEY_GET, which is
owner-only, because waiting consumes nothing and
all waiters wake together on input. There is no
timeout in this slice: a waiter with no input stays
waiting until input arrives, or until it is killed
with `kill`, which sets DEAD regardless of state.
A WAIT timeout (combining sleep's wake tick with
input wake) is a later piece, not in this version.

Kernel pieces. task.c gains TASK_WAIT_INPUT (4),
distinct from TASK_BLOCKED (2): BLOCKED means
"wake me at a tick" (sleep), WAIT_INPUT means
"wake me on input", and task_pick_next wakes
BLOCKED by time but skips WAIT_INPUT entirely, so
a waiting task never wakes by time, only by
task_wake_input_waiters, called from the keyboard
and mouse interrupt handlers. task_state_name
returns "waiting" for it, so `tasks` shows a waiter
as waiting, not sleeping or dead (code change; not
screenshot-proven, because typing `tasks` itself
wakes waiters, so a waiting task cannot be caught
in a tasks screenshot). keyboard.c gains
keyboard_pending (peek, not dequeue, for WAIT's
pre-check) and wakes waiters on a successfully
enqueued key only; a dropped key (queue full) does
not wake, because no new input was delivered.
mouse.c wakes waiters after a completed packet's
position and buttons are fully updated, so a woken
task reads the new state, not the old one; a
dropped first byte (resync) does not wake, while
an overflow packet still wakes, because buttons
are updated even when an axis is ignored. Wake is
wake-all, deliberate and simple: input is
broadcast (a key stays queued for whoever reads
it, mouse state is readable by anyone), so there
is no single right waiter to pick, and a herd of
two tasks is not a problem in a toy OS. Wake runs
in interrupt context with interrupts already off,
so it takes no lock and does no printing, heap,
or I/O, only state-flag writes.

Fault log. New file src/kernel/faultlog.c (Makefile
OBJS gains faultlog.o), 8 entries, ring buffer,
oldest overwritten first, each holding the
diagnostic string exactly as printed (without the
screen version's leading and trailing newlines),
plus the tick and the faulting task's id. Eight
entries at 64 bytes each live in BSS, no heap
allocation, which matters because the log is
written from fault handlers, where allocating
would be unsafe. No lock is taken, and none is
needed: log writes happen only in fault handlers,
which run with interrupts off, so no task switch
and no other log write can interleave with one.
Both fault paths log: generic exceptions in idt.c
and page faults in paging.c now build one
diagnostic string, call faultlog_add, then print
it, so log and screen agree exactly by
construction. The old piece-by-piece print helpers
(print_dec in idt.c, print_hex32 in paging.c)
were replaced by bounded buffer-append helpers,
so the build stays at 0 warnings. The shell gains
a `faults` command (help + dispatch, now 26 shell
commands), which calls faultlog_dump: "No faults
logged." when empty, else one line per entry,
oldest first, "tick <tick> task <id>: <diagnostic>".
Limits, stated plainly: the log is RAM only, no
persistence across reboots; a kernel fault still
halts after logging, so its entry is in RAM but
unreachable (the log is for user faults the OS
survives); 8 entries means a ninth fault drops
the oldest.

GUI. The loop now waits at the end of each frame
with the mouse state it just processed
(sys_wait(prev.x, prev.y, prev.buttons)), instead
of spinning, and the header comment says so.
Guarded by !quit: Esc or Quit must exit at once,
not wait for another input first. Program-icon
launch still exits directly via sys_exit inside
the loop, so it never reaches the wait. About
version updated to v0.10.3. Kernel banner and
shell version updated to "RingTail v0.10.3 -
Input wait + fault log".

Demos. waitdemo (272 bytes, _start 08000000):
prints "Waiting for input...", reads MOUSE_STATE,
waits with that state, prints "Woke up on input.",
exits. gfxfault (320 bytes, _start 08000000,
name exactly 8 characters for 8.3): GFX_BEGIN,
then prints from unmapped 0x09000000 like badptr,
so it faults in graphics mode; if it somehow does
not fault, it ends graphics cleanly before
exiting, rather than leaving the OS in graphics
mode with no program drawing. Both are run via
the shell, not via GUI icons: the full disk now
has 11 programs, past the GUI's 9-icon cap, so
waitdemo and gfxfault are not shown on the
desktop (see below).

Verification (screenshots
~/workspace/ringtail/screenshots-v103/).
Kernel 68,400 bytes, 0 warnings. File size is up
from 59,836, but most of that jump is not code:
rebuilding v0.10.2 from canonical today gives
59,836 exactly, confirming the toolchain is
unchanged, and section sizes show text +2,169
bytes (43,169 to 45,338), BSS +608 (44,180 to
44,788, the faultlog buffer), and the rest is
4KB section-alignment padding (linker script
BLOCK(4K)/ALIGN(4K)): .text crossed a page
boundary, pushing .rodata by 4KB, and rodata-area
growth pushed .data by another 4KB, so the file
size metric is page-quantized, not smooth. Real
code growth is about 2.2KB, plus 0.6KB BSS.
Host tests unchanged, 106 CHECKs, ALL TESTS
PASSED (WAIT and fault log are QEMU-only, like
LIST, SPAWN, and SEEK before them). Program
builds, all _start 08000000: hello 160, count
372, fault 52, crash 164, badptr 192, filedemo
1304, gui 4972, seekdemo 2876, listdemo 1344,
waitdemo 272, gfxfault 320. Full disk (hello,
count, fault, crash, badptr, filedemo, gui,
seekdemo, listdemo, waitdemo, gfxfault,
welcome.txt in that order): 12 files, 30
clusters, fsck.fat clean. LIST order matches
disk order, including WAITDEMO at 9 and GFXFAULT
at 10, and the GUI shows the first 9 programs
only, as designed.
Wait, key wake: v103wait-1 shows "Waiting for
input..." with no wake output after about 5
seconds of no input, proving WAIT blocks rather
than waking on time; v103wait-2 shows "Woke up
on input." after a key, followed by "unknown:
xversion", which is the documented WAIT-does-not-
consume behavior, not a bug: the waking key stays
queued, so the shell then types it before
"version". Wait, mouse wake: v103waitm-2 shows
"Woke up on input." after a mouse move, then a
clean "version" -> "RingTail v0.10.3 - Input
wait + fault log", because a mouse wake leaves
no key queued for the shell. Fault log:
v103fault-1 "No faults logged." on a fresh boot;
v103fault-3 after run fault, "tick 527 task 1:
PAGE FAULT addr=0x00000000 code=5 - task
killed.", matching the screen diagnostic exactly
plus tick and task id; v103fault-4 after run
gfxfault shows only "Started gfxfault as task
2" and a prompt, with NO fault diagnostic on
screen, proving the graphics-restore loss the log
exists to fix; v103fault-5 faults shows both
entries in order, "tick 527 task 1: PAGE FAULT
addr=0x00000000 code=5 - task killed." and "tick
934 task 2: PAGE FAULT addr=0x09000000 code=0 -
task killed." GUI regression with the wait loop:
v103gui-1 desktop, 9 green icons in LIST order,
top bar X=160 Y=100; v103gui-2 Welcome window
after a click, X=35 Y=43, "Welcome to
RingTail!", "Click me", "Clicked: 0";
v103gui-3 Esc exit, version v0.10.3, shell alive,
proving mouse-move, click, and Esc-key wakes all
work through WAIT. Regression: v103reg-1 listdemo
lists all 12 files, small/null-buffer checks
pass, spawn-hello succeeds, spawn-missing fails;
v103reg-2 seekdemo all checks pass; v103reg-4
filedemo writes/reads back "Hello from a
program!" and cat demo.txt matches. GRUB ISO:
v103iso-2 waitdemo mouse wake + version,
screenshot byte-identical to the direct-kernel
run (same SHA-256). Canonical verified from a
fresh copy before scratch deletion: kernel
68,400 bytes, host tests pass, canonical
waitdemo screenshot hash identical to scratch.
Scratch dirs (scratch-wait, scratch-canonical-
check) deleted after verification.

Honest limits (stated to T): WAIT has no timeout
yet; fault log holds 8 entries, RAM only, kernel-
halt entries unreachable; tasks "waiting" state
name is code-only, not screenshot-proven;
GUI still shows at most the first 9 programs, no
paging/scrolling, files with a dot are never
launchable from the desktop, test programs are
listed like any program, launching exits the
desktop, 64 KB cap unchanged, GUI still full-
redraws on any change (no damage regions), WAIT
wakes all waiters, text-mode WAIT shares keys
with the shell (a waking key stays queued).

Next: push v0.10.3 (awaiting T's word, do not
push unasked); then, as build candidates, a WAIT
timeout, and the other open items (64 KB cap
raise/streaming only when a file needs it, heap
from the memory map, VGA text split out of
kernel.c, real headers, UNDERSTANDING.md into a
repo docs folder, mock update past v0.9.0, now
four versions behind, unrequested).

v0.10.4 - Heap from memory map (patch, not minor: the
allocator is unchanged, only where the heap lives and
how big it is moved from a constant to a map-derived
value, with no new syscall or program, so this is
incremental in the seek/launcher/wait sense, and the
conservative choice under the version-climbing rule).

The heap used to be a fixed 4 MB at a fixed spot
after the kernel, which worked because QEMU's RAM
layout is known and forgiving, but it assumed RAM
instead of asking. Now boot asks GRUB for the
memory map and places the heap in it. Boot.asm
FLAGS went from 0x00 to 0x03: bit 1 (provide memory
information) makes the map a request, not a lucky
accident (GRUB in QEMU provided mem_upper anyway
with FLAGS 0x00, which is why kernel_main could
already use it, but the full map is only guaranteed
when requested), bit 0 (align boot modules) is the
conventional partner flag, harmless with no modules,
and the checksum recomputes automatically from
MAGIC + FLAGS.

Heap.c: HEAP_SIZE is gone as a constant. A static
heap_size variable, set by heap_init_at(start, size),
is now the bound kmalloc checks and heap_get_total
returns, so a larger heap actually allows larger
allocations and a smaller heap rejects sooner, and a
pre-init kmalloc still returns NULL safely
(free_list is 0). heap_init_at is the general init
(one free block, used 0). heap_init is kept for
host tests, as heap_init_at(align_up(_kernel_end,
4096), 4 MB), so the host heap and its CHECKs are
unchanged in behavior. heap_init_from_map(mbi),
guarded with RINGTAIL_KERNEL so the host link stays
clean (it uses print and hlt, which host tests do
not have), is what kernel boot now calls instead of
heap_init. It reads the multiboot info struct:
info[0] flags, info[11] mmap_length, info[12]
mmap_addr. If mbi is 0 or flags bit 6 (mmap valid)
is clear, it prints a diagnostic and halts, before
paging, while VGA text still works, like
paging_init's out-of-frames halt: a heap is
essential, and booting without one, or with a
silently wrong one, would be worse than stopping
with a message. Entries are walked by size + 4,
not a fixed stride, because the spec allows larger
entries. Each entry gives base (8 bytes), length
(8 bytes), type (4 bytes, 1 = usable RAM). Only
type 1 is usable; reserved, ACPI, and bad are
skipped, conservatively (ACPI reclaimable could be
reused after ACPI init, but RingTail has no ACPI,
so it stays reserved). Entries with a nonzero base
or length high word are skipped, not misread: this
is 32-bit with no PAE, and a 64-bit address cannot
be intersected with the 16 MB identity range
without 64-bit arithmetic. A base + length that
wraps past 4 GB is rejected outright, so it cannot
look like a tiny region. The heap goes in the
LARGEST usable region overlapping the kernel:
start is the later of the region start and
page-aligned kernel_end, capped end is the earlier
of the region end and 16 MB (the identity limit),
then minus FRAME_RESERVE 1 MB, giving end. Largest
wins, because usable RAM below 16 MB may be
fragmented, and the heap should take the biggest
usable piece, not the first. Smaller pieces and
gaps are wasted in this slice; a full physical
memory manager using every region is a later
piece, and QEMU's map is contiguous above 1 MB,
so nothing is wasted there. If the best region is
smaller than MIN_HEAP_SIZE 1 MB (disk buffers
about 82 KB, task stacks 4 KB each, ordinary
allocations, with room to spare), it halts with
"Not enough usable memory for heap".

Frame reserve, found by thinking through small
RAM before testing it, not by a test failure:
the first version capped heap_end at 15 MB only,
which leaves a small-RAM machine with only the
gap between the usable region's end and RAM top
for frames (128 KB in QEMU, zero on hardware
whose usable region ends exactly at RAM top,
which would make paging_init halt with no
frames). The fix subtracts the reserve from the
capped end in every case, so at least 1 MB of
frames (256 frames; kernel tables need 5, a user
task needs 2) is guaranteed on any machine that
boots, and on a large-RAM machine the heap still
ends at 15 MB, exactly as a cap would give. The
fix was verified by a re-run whose heap screenshot
is byte-identical to the pre-fix run (same SHA-256,
see below), proving large-RAM behavior did not
change, as designed.

Boot info is single-use, with one exception.
Kernel_main already read mem_upper before heap
placement, heap_init_from_map reads the whole map
before writing any heap, and nothing after heap
init reads the info struct, the command line, or
the modules again, so the heap writing over those
addresses afterwards destroys nothing the OS still
needs, except the shell's mmap_dump, which reads
the map array again from the saved address (see
its limit below). Kernel_main's old mem_upper
check (halt if less than 5 MB above 1 MB) was
removed, not moved: it was tied to the fixed 4 MB
heap that no longer exists, and the map-based
check in heap_init_from_map replaces it; a second,
less accurate halt against a total would only
duplicate it. Ram_top for frames is unchanged
(from mem_upper, capped to 16 MB inside
paging_init): frames still use a total, not the
map, in this slice. QEMU's map is contiguous below
16 MB, so the total and the map agree there, and
the limitation only matters on fragmented real
hardware, where a reserved hole below 16 MB could
be handed out as a frame. That is documented, not
fixed here, to keep this slice to the heap alone.
RAM above 16 MB is still unused entirely (heap
and frames are both capped there); using it needs
kernel page tables beyond the identity range, a
future piece, not this slice.

Shell, now 27 commands (was 26): new `mmap`
command, help line and dispatch branch after
`faults`, extern mmap_dump in shell.c. Mmap_dump
prints the saved map, one line per entry,
"base=0x........ len=0x........ type=<n>
usable/reserved", with high:low pairs when a base
or length high word is nonzero, so a skipped
above-4GB entry still shows honestly instead of
silently printing its low word. The address and
length are the ones saved at boot, not re-read
from the info struct, because the heap may have
overwritten the info struct itself by now. The
map array is still read from its original GRUB
address, which survives heap placement in QEMU
(proven by the correct dump in testing), but a
bootloader that placed the map array inside the
chosen heap region would corrupt a later dump:
placement itself is unaffected, because placement
reads the whole map before writing any heap, but
the dump would then print heap contents as if
they were a map. Copying the map to a safe buffer
at boot is a future hardening, not done here.
Kernel banner, shell version, and GUI About
window all say v0.10.4. No other GUI or syscall
code changed; program sizes are identical to
v0.10.3 (gui 4972, same version-string length),
all _start 08000000, disk unchanged (12 files,
30 clusters, fsck.fat clean).

Verification, screenshots in screenshots-v104/:
host tests 106 CHECKs, ALL TESTS PASSED, test
file untouched. Kernel 68,580 bytes (was 68,400,
+180), 0 warnings. 64 MB (harness -m 64): mmap
(v104mmap-1, zoom-verified, base + len = next
base throughout, no gaps) shows 6 entries:
base 0x00000000 len 0x0009FC00 type 1 usable,
base 0x0009FC00 len 0x00000400 type 2 reserved,
base 0x000F0000 len 0x00010000 type 2 reserved,
base 0x00100000 len 0x03EE0000 type 1 usable
(ends 0x03FE0000), base 0x03FE0000 len
0x00020000 type 2 reserved (ends 0x04000000,
64 MB), base 0xFFFC0000 len 0x00040000 type 2
reserved (BIOS area, ends 4 GB). Heap
(v104mmap-2): start 0x119000, total 14,577,664
bytes, used 81,968, free 14,495,680, free blocks
1, heap_end = start + total = 15,728,640 =
15 MB exactly, and used + free + 16 (header) =
total exactly. Malloc (v104mmap-3/5/6): 1,000,000
bytes at 0x12d040, heap used 1,081,984
(81,968 + 1,000,016), free 13,495,664; then
8,000,000 bytes at 0x221290, used 9,082,000,
free 5,495,648, arithmetic exact at every step,
free blocks 1 throughout. The 8 MB allocation
succeeding is the key proof the heap grew: with
the old 4 MB heap it would have returned NULL
(size over the heap bound). Re-run after the
reserve fix: heap screenshot byte-identical to
the pre-fix run (SHA-256 b982137c0954204e18769
cf84caa74a9d728005f938cb6600a37b63421a9d
35e39, v104mmap2-2 = v104mmap-2). 8 MB
(RT_MEM=8 harness variant, /tmp only, not
checked in): mmap usable [1 MB, 0x7E0000),
heap start 0x119000, total 6,057,984, used
81,968, free 5,976,000, free blocks 1, heap_end
0x6E0000 (usable end 0x7E0000 minus 1 MB
reserve), arithmetic exact, and run hello
succeeds ("Started hello as task 1", "Hello from
disk!"), proving the heap scales with RAM
instead of being hardcoded, and frames still
work with the smaller pool. GRUB ISO (v104iso-1/2):
heap numbers identical to the direct-kernel run
(start, total, used, free, free blocks all
match), version v0.10.4, so the FLAGS change did
not break ISO boot and GRUB provides the same
map as QEMU -kernel. The ISO screenshot hash
differs from the direct run only because the
screen contents differ (ISO run had no mmap
lines above the heap output, direct run did);
numbers match, and hash comparison does not
apply here, unlike v0.10.3 where the commands
were identical. Regression (v104reg, v104gui):
listdemo lists all 12 files, small/null-buffer
checks pass, spawn-hello succeeds, spawn-missing
fails; seekdemo all checks pass; filedemo writes
and reads back "Hello from a program!" and cat
demo.txt matches; run fault then faults shows
"tick 1193 task 5: PAGE FAULT addr=0x00000000
code=5 - task killed." (tick differs from
v0.10.3's 527/934 because timing differs, not
because the log changed); waitdemo mouse wake
then clean version v0.10.4; GUI Welcome window
after a click, X=35 Y=43, "Clicked: 0", Esc exit,
version v0.10.4, shell alive. All pass with the
new heap and the new frame pool ([15 MB, 16 MB)
at 64 MB).

Honest limits (stated to T): heap uses the
largest usable region only, smaller pieces and
gaps wasted; frames still use ram_top total, not
the map; above-4GB map entries skipped (32-bit,
no PAE); heap and frames both capped at 16 MB,
RAM above 16 MB unused; mmap_dump reads the map
from its original GRUB address, which survives
in QEMU but would corrupt a later dump if a
bootloader placed the map inside the heap region
(placement unaffected); boot info overwritten
after reading, by design, except the mmap_dump
case above; 64 KB cap unchanged; GUI still shows
at most the first 9 programs, no paging or
scrolling, files with a dot never launchable
from the desktop; WAIT still has no timeout.

Next: push v0.10.3 + v0.10.4 together, as one
batched push (awaiting T's word, do not push
unasked; remote is still at v0.10.2, local has
two versions unpushed, which T said is fine, not
urgent); then, as build candidates,
a WAIT timeout, frames from the memory map, and
the other open items (64 KB cap raise/streaming
only when a file needs it, VGA text split out of
kernel.c, real headers, UNDERSTANDING.md into a
repo docs folder, mock update past v0.9.0, now
five versions behind, unrequested).

---

v0.11.0 "Hardened" - audit and deep refactor

Every subsystem was reworked after a full audit. The old code mostly
worked on the happy path, but nearly every boundary was unchecked.

Kernel structure: real headers in src/include, with ringtail/abi.h as
the one user/kernel contract. All vectors go through a single
interrupt_dispatch. The GDT and TSS were rebuilt. Syscalls run with
interrupts enabled and return -errno. Kills are deferred to a safe
point. There is an idle task, and the shell is an ordinary kernel task
instead of running inside the keyboard path.

Memory: the first 1 GB is identity-mapped for the kernel, supervisor
only, with text and rodata read-only. Programs get 1-3 GB. ELF segments
are mapped with their own permissions, and the stack grows on demand to
1 MB with a guard gap. The frame allocator and the heap are both sized
from the memory map. Every user pointer goes through checked copy
routines.

Programs: real ELF loading with full header and segment validation,
argc/argv on the initial stack, and a small C runtime (crt0, ulib). The
x87 state is saved per task. Exit statuses are recorded (-1 killed,
-2 faulted), and join is killable. The tty belongs to a foreground job,
so Ctrl+C kills it, and `&` runs a job in the background.

Filesystem: diskfs.c was rewritten. The BPB is validated, and the FAT
and root directory are cached with dirty tracking. Data is written
before metadata, and write_file is copy-on-write. Open files are
ref-counted (unlink returns -EBUSY), and there is a mutex instead of
interrupts disabled. A file descriptor layer (file.c) handles O_CREAT,
O_TRUNC and O_APPEND and 64-bit-safe seek.

Shell: rewritten. It has a command table, a line editor with history,
Ctrl+C and Ctrl+L, and run with args and `&`. The free command only
accepts the shell's own allocations, and kill refuses to kill the shell.

GUI: double-buffered (one blit per frame), with the font fetched from
the kernel and a paged launcher. Launching a program drops to text mode,
waits for it, then returns.

Build and tests: one Makefile building into build/. The host tests were
rewritten for the new APIs (97 checks, a fault-injecting RAM disk, and
fsck.fat on the result). A new QEMU integration suite (51 checks) drives
the real ISO over serial: every program, crash isolation, Ctrl+C,
reboot persistence, poweroff, and fsck.fat plus mtools on the disk.

Bug the new tests caught on the way: diskfs_write_file left an empty
file behind when creating a new file failed with ENOSPC. Fixed.

Known limits: root directory only, 8.3 names, no SSE in programs, no
pipes or signals, no SMP, VGA text and mode 13h only.

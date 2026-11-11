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

## v0.12.0 "Octavo"

Octavo, the editor: a port of the Win32 Octavo to the terminal, keeping
its design (one buffer with caret and anchor offsets, soft word wrap
with hanging blanks, snapshot undo with typed runs grouped, word motion
by character class, find and replace that match case by default, the
two-line footer). It redraws by diffing a back buffer against what is on
screen, so a keystroke sends a few dozen bytes.

Kernel pieces it needed, each usable by any program:
- `sbrk` and a user malloc/calloc/realloc/free (first fit, both-way
  coalescing, bad frees end the program).
- Raw terminal mode with xterm key sequences, F1-F12 and Alt, plus
  TTY_NOSIG with a triple-Ctrl+C escape hatch. The mode resets when the
  foreground changes.
- ANSI escapes in the console (cursor motion, erase, save/restore, SGR
  colours, cursor hide/show), with blink turned off so bright
  backgrounds work.
- `save`: replace a file whole or not at all.
- `spawn` with an argument list.

Cleanup: the test and demo programs moved to tests/programs and only go
on the test disk. The shell lost ps, spawn, malloc, free, tick, colors,
mouse and gfx, and the kernel code only they used.

Tests: 97 host checks, 72 QEMU checks, including heap, raw keys, ANSI
output read back from VGA memory, and a scripted Octavo session.

Bug found on the way: SGR colours inherited brightness from the bright
white default, so red came out light red. Bold is now its own flag.


## v0.13.0 "Userland"

The shell moved out of the kernel. It is now SHELL on the disk, an
ordinary ring 3 program, and src/shell is gone. A kernel task, init,
starts it, gives it the terminal, waits for its job and starts a fresh
one when it ends (with a second's pause if it died at once, so a broken
shell cannot spin).

Syscalls the shell needed, each usable by any program:
- `tty_fg`: lend the terminal to a job you started, and take it back
  (with your terminal mode) once every job it was passed to is dead.
- `wait_job`: wait until every task in a job has ended.
- `kill`: stop a user task; kernel tasks answer EPERM.
- `readdir`, `kinfo` (version, mem, heap, tasks, faults, mmap, disk),
  `set_color` (refuses invisible text) and `power` (reboot, off).
- `spawn` takes options: a new job or an existing one to join, and a
  map for the child's descriptors 0-2. Children inherit copies of the
  parent's standard descriptors; fd 2 is now stderr.
- `open` understands O_EXCL.

The kernel's reports (task list, fault log, memory) are built into a
buffer with a new sbprintf instead of printed, and kinfo copies that
buffer out to the caller. The console's backspace now crosses back over a line wrap, so the
line editor can erase anywhere.

Shell: same commands, plus `exit`, typing a program's name without
`run`, and `echo` that keeps spacing. `sleep` became a program. The
tokenizer already knows `|`, `<`, `>` and `>>`; they are refused until
the next step.

Tests: 97 host checks, 83 QEMU checks (new: bare program names, sleep,
exit and respawn, kernel tasks refusing kill, the shell refusing to kill
itself by its real id).

## v0.14.0 "Plumbing"

Pipes and redirection. `a | b | c` runs up to eight commands joined by
pipes, all in one job, so Ctrl+C stops the whole pipeline. `< FILE`
feeds the first command, `> FILE` and `>> FILE` take the last one's
output. A built-in can start a pipeline or be redirected: the shell
points its own stdout at the pipe or file with dup2 while it runs, and
built-ins then leave out colour and screen-only lines (ls drops its
summary), so `ls | wc` counts files.

Kernel:
- `pipe`: a 4 KB ring buffer, refcounted per end across tasks. A read
  waits for data and returns 0 once no writer is left; a write waits
  for room and gets EPIPE once no reader is left. Both wake on kill.
- `dup2`, and EPIPE in the errno list. MAX_FDS moved into the ABI.
- Children inherit pipe ends through the spawn descriptor map, with
  the references counted, and the shell closes every end it does not
  need, so readers see end of file.
- The reaper remembers each task's job and parent, so a pipeline can
  still add to a job whose first program has already finished.
- Bug found by the new tests: the shell's line editor is raw, and
  handing the terminal from a raw job reset the console, so every
  program run cleared the screen. The reset now happens only when the
  terminal comes back (or init takes it after a shell dies).

New programs: `wc` and `grep [-i] TEXT` (plain text, not patterns).

Tests: 97 host checks, 100 QEMU checks. New: pipetest (ordering,
wrap-around, EOF, EPIPE, dup2, EMFILE, bad pointers, a 20000-byte
transfer through wc, joining a job after its leader ended), and shell
pipelines, every redirection, the error messages, and Ctrl+C on a
pipeline leaving no tasks behind.

Next idea: subdirectories.

## v0.15.0 "Burrow"

Subdirectories. `mkdir`, `rmdir`, `cd` and `pwd` in the shell, paths
everywhere a file name went before (`DOCS/A.TXT`, `../X`, `/WELCOME.TXT`),
and a prompt that shows where you are: `ringtail:/DOCS> `.

Kernel:
- diskfs rewritten around directory nodes. The root stays in memory;
  subdirectory sectors go through an 8-sector write-back cache, flushed
  after the FAT and the root. Every path call takes the directory to
  start from, so a task's current directory is just a node it holds a
  reference to. Anything referenced (an open file, someone's cwd) can't
  be deleted; rmdir gets EBUSY.
- Directories grow a cluster at a time; the root still has its fixed
  512 entries. `..` comes from the on-disk entry, and getcwd climbs it,
  finding each directory's name in its parent.
- A new directory's cluster, with `.` and `..`, is written straight to
  disk before the FAT and the parent's entry point at it, and freed
  clusters are dropped from the cache, so a crash can't leave a
  directory full of stale sectors.
- mkdir refuses a directory whose full path couldn't fit PATH_MAX (128),
  so getcwd can always answer.
- New syscalls: mkdir, rmdir, chdir, getcwd. readdir now takes a path
  and returns 1 per entry and 0 at the end, so a missing directory and
  the end of a listing are different answers. delete, save, open and
  spawn take paths. `list` (the old flat call) shows only files.
- New errnos: ENOTDIR, EISDIR, ENFILE, ENOTEMPTY.
- Bare program names are looked up in `/` whatever the cwd; a name
  with a `/` in it is resolved from the cwd.

Shell: `ls [dir]` shows directories first-class (blue `NAME/ <dir>` on
the terminal) and counts them in the summary. `rm` on a directory points
at rmdir. Octavo takes paths in Open and Save as, and the desktop always
lists programs from `/`.

Tests: 152 host checks (mkdir and rmdir rules, every path form, a
directory growing past one cluster, a full disk and a full root, an I/O
error during mkdir, a tree surviving a remount, the depth limit, and a
70-file directory for fsck), 123 QEMU checks (every new command and
error, programs run from inside a directory, a nested file surviving a
reboot, and mtools reading it afterwards).

Next ideas: rename/mv and cp, then long file names or a second disk.

## v0.16.0 "Packrat"

Moving things around. `mv` in the shell renames or moves files and
whole directories, and a new `cp` program copies files.

Kernel:
- `rename(from, to)`: a new entry goes in, the old one goes away, and a
  moved directory's `..` is pointed at its new parent, all in one
  commit. The target must not exist (EEXIST; `mv` says to rm it
  first). A directory can't be moved into itself or below (EINVAL),
  and a move is refused when some directory in the moved tree would
  end up with a path past PATH_MAX, found by an iterative walk of the
  tree. An open file stays open: its node follows the entry.
- `stat(path, dirent_t*)`, and dirent_t gained `id`: two paths with the
  same id are the same file or directory. cp uses it to refuse copying
  a file onto itself, which would otherwise truncate it to nothing.
- The subdirectory sector cache grew from 8 to 16 sectors, since a
  rename dirties more of them.

User side: `mv SRC DST` and `mv SRC... DIR`, into a directory when the
target is one (`dest_path` in ulib, shared with cp). `cp` streams 4 KB
at a time, replaces an existing target, refuses directories, and
removes a target it created if the copy fails partway.

Tests: 188 host checks (rename in place and across directories, an
open file through a move, directory moves and their `..`, the loop and
depth checks, stat and ids, and a move on 2 KB clusters for fsck),
136 QEMU checks (mv and cp, every refusal, a copied program running).

Next: long file names.

## v0.17.0 "Longtail"

Long file names. Names keep their case, run to 255 characters, and can
have spaces and several dots; `cd "My Docs"` works and the prompt shows
it. Lookups ignore case, and match the short alias too.

Kernel:
- VFAT long-name slots: written before the short entry, last piece
  first, with the alias's checksum. Read back only when the run of
  slots is unbroken and the checksum matches; a broken one, or one with
  characters outside printable ASCII, falls back to the short name.
- Short aliases the Windows way: uppercase, spaces and all but the last
  dot dropped, other unfit characters made `_`, then the lowest free
  `~N` (`REPORT~9`, then `REPOR~10`). A name that only loses its case
  (`ReadMe.txt`) keeps the plain alias `README.TXT`, and one that fits
  8.3 in a single case per part (`notes.txt`) gets no long name at all,
  just the NT lowercase bits.
- Free runs of slots for a long name are found across sectors, and a
  subdirectory grows by clusters to fit one. Deleting frees every slot.
- rename handles changing only the case, keeping the entry's alias.
- NAME_MAX 255 and PATH_MAX 256 in the ABI, and dirent_t's name grew
  to match. `strcasecmp` in the shared lib, `%*s` in printf.

User side: `ls` widens its name column to the longest name (up to 32),
the shell says "not a valid file name" for a bad name instead of
"invalid argument", Octavo saves new documents as `.lemur` and hylic
ones as `.txt`, and the desktop shows long names cut to seven letters
and a `~`.

Tests: 229 host checks (aliases and tails, case bits, the 255-character
name across sectors and across clusters, reuse of freed slots, case-only
and long/short renames, bad checksums and non-ASCII slots falling back,
the PATH_MAX limit with long names, and long names in the image fsck
checks, which mtools also lists correctly), 148 QEMU checks (long names
in the shell, a long-named file mtools wrote before boot, and one the
guest wrote that mtools reads after).

## v0.18.0 "Tabby"

Tab completion in the shell. Shell only; no kernel changes.

- Tab completes the word at the end of the line. The first word of a
  command (and the word after `run`) is a command: a built-in, or a
  program in the root, offered in lower case. Any other word is a path,
  completed against the directory it names. `cd` and `rmdir` offer
  directories only; a word after `<`, `>` or `>>` is a path.
- Matching ignores case, like the disk; a finished name takes the
  disk's case. With several matches Tab fills in what they share, and
  when it can go no further it lists them in columns.
- A name with a blank or another special character is wrapped in quotes
  as it goes in (`cd tab<Tab>` gives `"Tab Test/`); a finished file
  closes the quote and adds a space, a directory leaves the quote open
  and adds `/` so the next Tab goes inside it.
- Up to 128 matches are collected; past that the listing says so.
- The editor still has no cursor in the middle of a line, so
  completion always works at the end.

Tests: 11 new QEMU checks (files, quoted names across directories,
programs, built-ins, `run`, `cd`, redirection targets, the listing).
It first failed on a loop that treated the readdir result as 0 for
success; readdir returns 1 per entry.

## v0.19.0 "Timekeeper"

File timestamps from the CMOS clock.

Kernel:
- `src/drivers/rtc.c` reads the CMOS clock (BCD or binary, 12 or 24
  hour, waiting out the once-a-second update and re-reading until two
  reads agree). The year is 2000 plus the chip's. `fs_clock` turns it
  into FAT date and time words for the filesystem.
- diskfs stamps created, modified and accessed fields when an entry is
  made (file, directory, `.` and `..`, and `save` creating a file), and
  the modified fields when contents change (pwrite, truncate, replace).
  Rename keeps them. The filesystem takes the clock through `fs_clock`,
  so the host tests supply their own.
- `dirent_t` gained `mdate` and `mtime` (FAT packing; 0 if never
  stamped). New syscall `SYS_TIME` (45) returns the clock as `rtc_time_t`.
- mkdisk.py stamps every file with the build time (`SOURCE_DATE_EPOCH`
  if set).

Shell: `ls` shows "YYYY-MM-DD HH:MM" after the size ("-" for an
unstamped entry); new `date` command.

Limits: no time zone (QEMU's CMOS is UTC), no daylight saving, no
access-date updates on read, no setting the clock.

Tests: 15 new host checks (stamps on create, write, truncate, replace,
mkdir, readdir, rename; unstamped entries; raw created/accessed fields)
and 4 QEMU checks (date against the host's UTC, ls on a new file, the
image's files, and mdir reading the guest's stamps). Host 244, QEMU 163.

## v0.20.0 "Cursor"

A cursor in the shell's line editor. Shell only; no kernel changes.

- Left, Right, Home, End and Delete move or edit anywhere in the line;
  typing inserts at the cursor and Backspace deletes before it.
  Ctrl+Left/Right and Alt+B/F go a word at a time. Ctrl+A/E/B/F match
  Home/End/Left/Right. Ctrl+W deletes the word before the cursor,
  Ctrl+K from the cursor to the end, Ctrl+U the whole line.
- getkey decodes `ESC [ param ; modifier final` properly (and the
  `ESC [ 1 ~` / `4 ~` / `ESC O H` spellings other terminals send).
- Edits repaint only the text after the cursor, blank what the line no
  longer covers and step back with backspaces, which the console
  carries across a wrapped line. Up/Down and Tab jump to the end of
  the line first; completion still works on the end only.

Tests: 24 QEMU checks. They replay the serial output onto a one-line
screen model and check the text and cursor column after each edit, run
the edited line, and read the VGA memory for a line wrapped across two
rows. (The model caught a wrong expectation of mine, not a shell bug.)

## Names: Saklas and Kenoma

The shell is now Saklas (disk name SAKLAS, `src/programs/saklas.c`) and
the desktop is Kenoma (`kenoma.c`; `run kenoma`). Both are Gnostic: the
Saklas is the fool, a name for the demiurge, and the kenoma is the
visible world the archons rule. Names were checked against GitHub repo
names for anything over 100 stars (Archon and Gam were turned down,
Archon as too solemn and Gam for the 4k-star Google Workspace CLI).
Octavo stays as it is. No behaviour changed.

## v0.21.0 "Gauge"

A system monitor, and the one syscall it needed.

- **gauge** (`src/programs/gauge.c`): a text-mode display of frames, heap
  and disk usage as bars, a one-line task summary, and the task table
  with states coloured (running, ready, blocked). The title row has the
  version, uptime and the clock. Redrawn every second; q, Esc or Ctrl+C
  quits, p pauses, + and - step the rate between 0.25 and 5 seconds.
  `gauge -1` prints a single plain-text snapshot (no escapes, `#` bars)
  for pipes and scripts. It reads the kernel's `kinfo` text, so there is
  no new reporting code in the kernel, and it shows up in Kenoma's
  launcher on its own.
- **SYS_POLL(ticks)**: waits up to that many ticks for terminal input for
  the caller (capped at an hour) and returns 1 if some is waiting, 0 if
  the time ran out, -EINTR if the task was killed. It reads nothing; the
  program then calls `read`. Without it a program could block on a key or
  sleep, but not both, so a timed display had no way to notice `q` before
  its next refresh.
- Each frame is built whole, every row positioned with an escape and
  padded to the screen's width instead of erased first, and written with
  one call, so nothing flickers.

Wrinkle: keys typed together arrive in one `read`, so the key loop goes
through every byte it gets (an early version looked at single-byte reads
only and dropped `++`). Also, with nobody reading the serial port a
redraw crawls (the console waits on the UART for every character),
which a test harness has to avoid by draining it while a display runs.

Tests: 27 new QEMU checks. polldemo checks the poll timeout and its
length, the instant return for 0, early return on a key, and being
killed mid-poll. For gauge: the snapshot's numbers, task rows and count,
piping, the usage error, the live screen (title colours, bars, footer),
refreshing by itself, pause and resume, the rate keys, and q and Ctrl+C
leaving a cooked terminal. Host 244, QEMU 215.

## A syscall fuzzer

No version bump: it is a test, `tests/programs/fuzz.c`. It runs inside
the guest, deterministic for a given seed (`fuzz [seed [calls [verbose-from]]]`),
and calls every non-blocking system call with hostile arguments: kernel
addresses, read-only code, pointers that straddle a page or wrap past
4 GB, huge lengths, bad descriptors and flags, awkward paths (renaming
a directory into itself, `..`, a name past PATH_MAX). A second phase
does the same to the graphics calls while it holds the display. The
oracle is crude on purpose: the kernel must not panic or hang, and the
kernel heap must come back to where it started. The QEMU suite runs
three seeds and then checks `mem` is unchanged and that its scratch
directory is gone; fsck at the end covers the filesystem.

Result: 39 seeds of about 6000 calls each found nothing. That is only
worth something if the fuzzer can see bugs, so it was checked against
two deliberately broken kernels, both reverted: dropping the wrap-around
test in `user_range_ok` (a kernel panic on the first seed) and removing
a `kfree` in `sys_kinfo` (a heap-loss failure on seed 1).

Left out of the random mix because they block or end the program: exit,
power, join, wait_job, kill and tty_fg on real tasks, reading the console,
pipe, and spawning real programs. Those are covered by their own tests
only.

Two things the fuzzer taught about itself: it must close every
descriptor before cleaning up (an open file cannot be deleted, so a
stray dup2 left its scratch directory behind), and its own verdict must
not use the check() counters, since a legitimate stray write can land
on them. Host 244, QEMU 220.

## Other CPUs and another emulator

No code change, only findings (and `Machine` in `tests/qemu_test.py` now
takes `cpu`, `mem` and `machine`). The ISO was booted outside the default
QEMU setup to get closer to what real hardware will do.

QEMU: pentium2 and newer CPUs, 4 MB to 1 GB of RAM and the i440fx machine
all boot and run the suite. Three things do not work:

- 486 and pentium CPUs print nothing. Cause (confirmed): the build uses
  `-march=i686`, so the compiler emits `cmov` (92 of them in the kernel,
  first hit in `strlcpy`), which is an invalid opcode before the IDT
  exists, so the CPU triple-faults and resets. Rebuilding with
  `-march=i586` or `-march=i486` removes every `cmov` and the kernel
  boots and passes the fuzzer on Bochs' pentium model. Not applied.
- q35 boots the kernel but sees no disk: its disk is AHCI and the driver
  only speaks legacy ATA PIO at 0x1F0. A real fix is an AHCI driver.
- Above about 1 GB of RAM is unused, since user space starts at 0x40000000.

Bochs 2.7, a separate x86 implementation, with `BIOS-bochs-legacy` (the
newer Bochs BIOS triple-faults inside itself before reaching GRUB, so that
one is Bochs' problem): boots to the prompt and runs `version`, `mem`,
`run hello`, `gauge -1` and `run fuzz 1 1500` on p2_klamath, p3_katmai,
core2_penryn_t9600 and corei7_haswell_4770, all clean, `mem` unchanged
afterwards. The Debian Bochs is a debugger build: it sits at a prompt
until it is sent `c` on stdin, and with stdin closed it quits at time 0.

None of this is real hardware: no ACPI quirks, no real PS/2 or VGA
timing, no SMM. It rules out "only works in QEMU", not "works on my board".

## v0.22.0 "Sonar"

The network, first milestone: a card driver and enough of IPv4 to ping
and be pinged.

- **PCI** (`src/drivers/pci.c`): configuration mechanism #1, a brute-force
  scan of every bus, device and function at boot, up to 64 devices with
  their ids, class, interrupt line and BARs. The `pci` built-in lists them
  with a vendor and class name.
- **Device window**: a 4 MB range at 0xC0000000 for memory-mapped
  registers, one page table shared by every address space, supervisor
  only and uncached. `mmio_map` hands out pieces of it; nothing is ever
  unmapped, since cards do not go away.
- **e1000** (`src/drivers/e1000.c`): the 82540EM that QEMU gives by default,
  plus the 82545EM, 82544GC and 82543GC, which cover VMware's and
  VirtualBox's choices (the 82543GC is untested: QEMU has no model of it).
  Reset, link up, MAC from the receive address registers or the EEPROM,
  a 32 x 2 KB receive ring and an 8-buffer transmit ring from the frame
  pool (about 180 KB), legacy descriptors, and an interrupt that only wakes
  the net task. Without a usable IRQ line it polls every tick instead.
- **The stack** (`src/net/net.c`): one kernel task, `net`, drains the ring
  and handles each frame under `net_lock`. Ethernet, ARP (a 16-entry table,
  ten-minute lifetime, learned only from requests aimed at us so the
  table cannot be filled by everyone else's chatter), IPv4 (version,
  header length, total length against the frame, checksum, fragments
  dropped, no options read but skipped correctly) and ICMP (echo replies
  sent straight back to the frame's sender, no answers to broadcast
  pings, replies and routers' unreachable messages matched to waiting
  pings). The address is fixed: 10.0.2.15/24, gateway 10.0.2.2.
- **SYS_PING** (47): sends one echo request and waits for its reply, the
  timeout, a router's unreachable message or Ctrl+C. Returns the round
  trip in microseconds and the reply's TTL, or ENETDOWN, EHOSTUNREACH,
  ENETUNREACH, ETIMEDOUT, EIO (the data came back different) or EINVAL.
  Those four network errnos are new, with their strerror text.
- **ping** (`src/programs/ping.c`): `ping [-c COUNT] [-s SIZE] [-W MS] ADDRESS`,
  a second apart, Linux-style lines and summary, `-c 0` until Ctrl+C. Exit
  status 0 if anything answered, 1 if nothing did, 2 for a usage error.
- **net** built-in (KINFO_NET): the card, its IRQ and interrupt count, link
  state, address, frame counters (in, out, dropped, for others,
  fragments, bad at the card), pings answered and the ARP table.
- **Timing**: the PIT now runs in mode 2 (rate generator) instead of
  mode 3, the same 100 Hz, but its counter runs down once per tick, so
  `timer_micros()` can read it. A rollover whose interrupt has not been
  taken yet (interrupts are off while reading) is caught from the PIC's
  request register. `task_block_timeout` lets a task wait on a channel
  with a deadline; the ARP and ping waits use it.

Wrinkles. The first ping after boot failed, every time. The cause was
QEMU's e1000, which holds received frames for a second after the receiver
is switched on, and ARP, which then asked three times in one second and
gave up. ARP now waits a second between its three requests, as Linux does
(real cards take that long to negotiate a link anyway), and a ping's
reply timeout starts when the request goes out, not before the ARP lookup.
The flood test wedged the network once, but in QEMU, not the kernel: its
socket backend takes a zero-length datagram as the far end closing and
stops reading, so the fuzzer never sends an empty frame. Frames shorter
than an Ethernet header do reach the driver (QEMU pads only some of them)
and are counted as bad at the card.

A race that was not the network's. init started the shell and then gave
it the terminal; when a timer tick let the new shell run first, it
printed its prompt, read the terminal as a background job (-EIO), took
that as having no terminal, and ended, so init started another one. The
suite caught it once, on the no-card boot, as every later command's output
arriving one command late. The shell had the same race with every
program it ran in the foreground. Now a task can be created held, and
`SPAWN_FOREGROUND` hands the child's job the terminal before releasing
it; init does the same for the shell. Only the terminal's owner may use
the flag. `tests/programs/fgspawn.c` checks the child owns the terminal
at its first instruction twenty times running, that the parent gets it
back, and that a background job is refused.

Tests. In the main session: the banner and `pci`, `ping` to the slirp
gateway (counts, TTL and round-trip format, 0 and 1472 bytes), `net`,
host unreachable for a silent neighbour after about three seconds, no
loopback, usage errors and bad addresses, multicast refused, Ctrl+C on an
endless ping, and `mem` unchanged. Then a second boot on a socket network
whose only other host is `tests/net_peer.py`, which writes frames byte by
byte. It checks the ARP reply field by field, pings of 0, 1, 33, 56 and
1472 bytes with their data and both checksums, IP options skipped, frame
padding not taken for data, and ten packets that must be ignored, with
the counters showing which way each went. It answers the guest's pings to
a host and through a router, then stays silent, sends a host unreachable
and damages the data, each with its own message. Last, 3000 frames of
generated garbage (noise, flipped bytes, truncations, IP headers with
lying lengths but good checksums, every ICMP type) fast enough to overrun
the ring, after which it still answers, the shell works and no memory is
gone. Short boots cover the 82545EM, the 82544GC and no card at all.
Host 244, QEMU 293.

## v0.23.0 "Switchboard"

The network, second milestone: UDP, an address from DHCP, and names.
`ping example.com` works.

- **Loopback**: anything sent to 127/8 or the machine's own address is
  queued back to the net task instead of the card, and comes in as if it
  had arrived. It works with no card at all. `net` counts it.
- **UDP sockets** (`src/net/udp.c`): a new descriptor kind, made by
  SYS_SOCKET (48) and used with SYS_BIND (49), SYS_SENDTO (50) and
  SYS_RECVFROM (51), which take a `udp_msg_t` (address, port, buffer,
  length, timeout in ms: 0 polls, -1 waits). 32 sockets in all, 16
  datagrams queued on each and the rest dropped and counted. Bind 0, or
  sending unbound, takes a random free port from 49152 up; 68 belongs to
  the DHCP client. A datagram for a closed port gets ICMP port
  unreachable, unless it was a broadcast. Checksums are checked when the
  sender gave one and always sent. `dup2` and spawn share a socket; the
  last close frees its port. New errnos: ENOTSOCK, EMSGSIZE, EADDRINUSE,
  EADDRNOTAVAIL.
- **DHCP client** (`src/net/dhcp.c`, the message code in `dhcpmsg.c`):
  RFC 2131 in the net task. DISCOVER with backoff from 4 to 64 s, the
  first good offer taken, four tries at REQUEST, then BOUND until T1;
  RENEWING asks the server straight, REBINDING asks everyone, and a lease
  that runs out or a NAK starts over (NAKs make it wait 2, 4, 8... s). It
  asks for its last leased address back. Mask, router, DNS servers,
  domain, lease, T1 and T2 are taken, each checked (a router off the
  network or a domain with odd bytes is ignored). Times are 64-bit ticks.
  init waits up to five seconds for a lease before starting the shell and
  prints it: `Network: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 (DHCP)`.
- **SYS_NETCONF** (52): GET reads the address, mask, gateway, DNS
  servers, domain and DHCP state; STATIC sets them by hand, checking they
  fit together, and stops DHCP; DHCP starts it again. In Saklas: `net`
  shows it all, `net static ADDR/BITS [GW|none] [DNS]` and `net dhcp`.
- **The user library is an archive** (`libu.a`), so a program links only
  what it calls. Every program shrank; hello went to 14736 bytes.
- **DNS resolver** (`src/programs/lib/dns.c` over `dnsmsg.c`): A and PTR
  questions with RD set, to each configured server in turn, two tries of
  2 s with the same ID. An answer counts only from that server's address
  and port, with the query's ID and the same question; others are
  ignored. Aliases are followed through one answer and across new
  questions, at most 8. A name with no dot is tried under the search
  domain first. A dotted address is its own answer, `localhost` and 127/8
  need no server, and a number that is not an address is a bad name.
  Names in answers are escaped (`\DDD`, `\.`) before anyone prints them.
  No cache and no TCP: a truncated answer is an error.
- **host** (`src/programs/host.c`): `host [-p PORT] NAME [SERVER]`, in the
  words BIND's host uses. **ping** takes names now.
- **udptest** (`tests/programs/udptest.c`): 38 checks over loopback, and
  `udptest echo PORT N` for the peer tests.

Wrinkles. The fuzzer left a directory called `670766666666` in the root,
which the next test's `ls` tripped over. It had wandered out of its
scratch directory with `chdir("..")` and called `mkdir` on a pointer that
happened to land in the font's bitmap; the new socket calls had shifted
its random sequence onto that path. The kernel did as it was told. The
fuzzer now lists the root before it starts and removes anything new
afterwards. It also printed megabytes of zeros once: when its print
guard moved a wild pointer into its scratch buffer, it kept the wild
length. `net dhcp` after `net static` asked the DHCP server for the
typed-in address (slirp grants whatever it is asked for), which is not
what a lease renewal means; it now asks for the last address DHCP gave.
A mutation check on the DNS parser showed the rule that compression
pointers only jump backwards is the only thing stopping a pure pointer
loop, so the host tests hold it to that.

Tests. Host: `tests/test_netmsg.c`, the DHCP and DNS message code under
ASan and UBSan, with exact-size copies so a one-byte overread is caught,
plus 200000 mutated DHCP messages and 300000 DNS ones. QEMU, user
network: the DHCP banner and `net` lines, loopback pings, `ping
localhost`, names refused as invalid, `udptest`, and `host` against
`tests/dns_server.py` on the host (addresses, duplicates and other
classes dropped, aliases in one answer and across two, capitals, forged
IDs and questions, escape codes, NXDOMAIN, no data, SERVFAIL, REFUSED,
alias loops, truncation, a compression loop, a short A record, a retry
after 2 s and giving up after 4, reverse lookups through an RFC 2317
alias), sockets closed and memory unchanged after, then `net static`
with its errors and `net dhcp`. The socket calls joined the fuzzer.
Peer network: `net_peer.py` is now the DHCP and DNS server. It checks
DISCOVER and REQUEST field by field, renewal at T1 to the server, rebind
at T2 by broadcast with the same xid, a lease that runs out (address and
DNS gone, the old address asked for again), NAKs with growing waits,
five offers and an ACK that must be passed over, and an address set by
hand. UDP: port unreachable quoting the datagram, silence for broadcasts,
bad checksums and lying lengths, and `udptest echo` returning a normal,
an unchecked, a 1472-byte and an empty datagram. DNS: the search domain,
a trailing dot, `ping printer`, answers from the wrong address or port
ignored, random source ports, and a failing first server. The garbage
flood is 4000 frames now, with UDP and DHCP-shaped ones among them.
Host 244 + 82, QEMU 419.

## v0.24.0 "Handshake"

TCP, and a page fetched over HTTP from inside RingTail.

The engine (`src/net/tcp.c`) is pure protocol and runs the same in the
kernel and on the host: the RFC 9293 state machine with the MSS option,
RFC 6298 retransmission timing (SYNs at 0, 1, 3, 7 and 15 s, giving up
at 31; eight retries on an open connection), RFC 5681 slow start and
congestion avoidance from a ten-segment first window, NewReno fast
recovery, zero-window probes, 40 ms delayed ACKs, up to four
out-of-order gaps held in the receive buffer, TIME-WAIT of 30 s, and the
RFC 5961 checks (a reset or SYN not exactly in place gets a challenge
ACK). Connections buffer 16 KB each way; a socket closed with data still
going out keeps sending it in the background. ICMP unreachables end a
connection that is still opening, and losing the address resets every
connection on it.

The kernel side (`src/net/tcpsock.c`) has the calls under the net lock.
A task that waits sleeps on the connection; the engine only notes whom
to wake, and the wakeups go out after the call, so nobody wakes to half
a change. New syscalls: `listen`, `accept` and `connect` (both with a
timeout), `shutdown`, `sockaddr`, `pollfd` and `fdtype`. `socket` takes
SOCK_STREAM. TCP sockets read and write like files, `dup2` and
inheritance work, and `pollfd` waits on any mix of sockets, pipes and
the terminal: pipes, the tty, UDP and TCP all tell it when something
changed. `net` lists TCP's counters and every connection.

Programs: `fetch [-i] [-o FILE] URL` speaks HTTP/1.0, follows up to five
redirects (absolute, //host, /path and relative), reports an error
status or a body shorter than its Content-Length, gives up after 30 s of
silence, and deletes a half-saved file. `nc` connects or listens; on a
terminal it edits the line itself in raw mode so that what arrives is
printed above it, Enter sends, Ctrl+D on an empty line ends the sending
side. User code dials names through `lib/dial.c`, which tries each
address DNS gave.

Tests. Host: `tests/test_tcp.c` runs two engines against each other and
against hand-made segments on a simulated network with loss, reordering,
duplication and a clock it controls: the handshake and every way of
closing, simultaneous open and close, resets and the RFC 5961 cases,
retransmission and backoff, fast retransmit and recovery, window
updates and probes, out-of-order delivery, sequence wraparound, the
connection table running out, and bulk transfers checked byte for byte.
QEMU: `tcptest` (errors, accept timing, a 256 KB transfer each way
through poll, shutdown, resets, the backlog, a full descriptor table,
a connection to our own address, pollfd on pipes), also on a machine
with no card. Against the host: `fetch` of a page, three kinds of
redirect, a redirect loop, a 404, a short body, https refused, and
300000 bytes saved with -o; `nc` through an echo server and to a closed
port. The new calls joined the fuzzer.
Host 244 + 82 + 163, QEMU 436.

## v0.24.1 "Handshake"

`httpd [-p PORT] [-q] [DIR]`, the first program to use `listen` and
`accept` for real. It answers GET and HEAD over HTTP/1.0, one connection
at a time: a file with its length and a type guessed from the extension,
a directory's index.html or else a generated listing (names escaped for
HTML and for URLs), a 301 for a directory named without its slash, and
404, 400, 405, 403 and 414 for what they say. The path is percent-decoded
and every part checked, so `..` however it is written (plain, `%2e%2e`,
or a `%2f` smuggled into a name) gets a 403 and nothing outside DIR can
be reached. A client that has not finished its request after 10 s gets a
408, and one that half-closes early is answered with what it sent.
Requests are logged to the screen unless -q.

Tested on the guest's own loopback with `fetch` and `nc`: a file, headers,
index.html, the listing, the redirect, a name with a space, a 404, four
spellings of `..`, a 300000-byte file, garbage, and the server still
answering afterwards. By hand through QEMU's port forwarding with curl:
the same, plus HEAD, POST, a byte-identical big file, and a client that
never finishes its request. QEMU 460.

## v0.24.2 "Handshake"

`head`, `tail` and `more`, with the standard names and flags.

`head [-n N | -c N] [-q] [FILE...]` stops reading as soon as it has
enough, so `cat big | head` does not wait for big. `tail [-n [+]N | -c [+]N]`
reads to the end but keeps only the lines or bytes it was asked for (it
trims once it holds 64 KB more than that), so a 300000-byte file costs
little memory; `+N` streams everything from the Nth line or byte. Neither
has -f: nothing here can say when a file has grown. `head -n -N` (GNU) is
not supported.

`more [FILE...]` pages at 80x25. Counting screens means knowing what the
console will do with each byte, so `more` carries a small model of it:
tabs to the next multiple of 8, the last column wrapping only when another
character arrives, and escape sequences taking no room. When the next
character would wrap into the prompt's row, `more` does the wrap itself
and then asks, so a screen is never one line short or long. Space and PgDn
give a screen, Enter and Down a line, q a quit; plain files show a
percentage.

The kernel gained one thing for it: opening `/dev/tty` (`DEV_TTY`) gives
the terminal itself, O_RDONLY for keys and O_WRONLY for output, whatever
descriptors 0 and 1 point at. That is what lets a pager at the end of a
pipe wait for a key. It is not a file and hides any file of that name;
O_RDWR and the creating flags are refused with EINVAL.

The terminal is raw only while the prompt waits for a key, with TTY_NOSIG
so Ctrl+C arrives as a byte and means quit. The first version stayed raw
for the whole run, and Ctrl+C then killed a raw job, which makes the
kernel wipe the screen, taking the page being read with it. Between
prompts the terminal is cooked, so Ctrl+C still kills `more` and the
pipeline behind it if the input has stalled. When output is not the
screen, or there is no terminal to ask, `more` copies like cat.

Known limits: no scrolling back and no search in `more`, which counts
rows from where it started, as the classic one does. `help | more` pages
only the command lines, because `help` leaves its footer off when its
output is not a terminal.

Tested in QEMU: head and tail on counts, `-N`, `+N`, bytes, several files,
headings and -q, stdin, empty input, a missing file and bad numbers, plus
a 300000-byte file through both; `more` on a screen exactly full, one
over, a line of exactly 80 columns, tabs, escape sequences, files
without a final newline, two files, a pipe, redirected output, Enter,
Down, PgDn, q and Ctrl+C (screen intact afterwards); `/dev/tty` from a
program with its descriptors redirected, and its refusals.
Host 244 + 82 + 163, QEMU 527.

## v0.24.3 "Handshake"

`sort` and `uniq`, with the standard names and flags.

`sort [-bcfnrsu] [-k F1[,F2]] [-t SEP] [-o FILE] [FILE...]` reads every
input into one buffer, indexes the lines (24 bytes each, no allocation per
line, which matters because the user malloc is first-fit), and runs a
stable merge sort. Order is bytewise; -f folds to upper case as GNU does,
-n reads blanks, a minus sign, digits and a fraction and compares them
digit by digit so no number overflows, and anything that is not a number
counts as zero. Lines equal on the key then go by the whole line, as in
GNU sort, unless -s or -u is given; -u keeps the first of each run in
input order. -c reports the first disorder with its line number and exits
1, -o may name an input because nothing is written until everything is
read, and options may follow the files. There is one key: `-k2,2n` is
the same as `-k 2,2 -n`, character offsets in a key (`-k 1.2`) and a
second -k are refused with a message. A last line without a newline gets
one; a file that cannot be read ends sort with status 2 and no output.

`uniq [-c | -d | -u] [-i] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]` streams
two lines at a time through buffers that grow, so line length and input
size are unlimited. It keeps the first line of each run, `-c` pads the
count to 7 columns like GNU, and it flushes before it waits for more
input, so it can sit at the end of a slow pipe.

Neither changes the kernel. Tested in QEMU against Python reference
implementations of the same ordering: every flag on small files (case,
numbers with blanks, signs, fractions, junk and negative zero, keys,
separators, leading blanks, -s and -u first-of-run, -c and -o including
over an input), their errors, and then 6000 lines sorted four ways and
checked with `sort -c`, a line count and eight lines read back from each
result, plus a 300000-byte binary file through three sort modes and long
lines through uniq.
Host 244 + 82 + 163, QEMU 599.


## v0.24.4 "Handshake"

A multi-file `cat`, `hexdump`, and `df`.

`cat` is a Saklas built-in, so it still cannot read standard input or follow a
`|`; what changed is that `cat a b c` now shows the files in a row (before it
took only the first). A file that cannot be read is reported and the rest still
follow. The one behaviour change: on the screen a last line without a newline
still gets one, but into a pipe or a file the bytes now pass through exactly,
so `cat a b > c` is a true join. It used to add a newline there. This is what
lets a test (or a person) build a multi-line file in the guest.

`hexdump [-C | -x] [-v] [-n LENGTH] [-s OFFSET] [FILE...]` treats its inputs as
one stream. The default and -x are util-linux's 16-bit words, low byte first,
with an odd last byte as `00xx`; -C is the canonical form with the text between
bars. A line equal to the one before is replaced by one `*` unless -v is given,
and a closing line carries the final offset. -s steps over whole plain files
without reading them and seeks into the one it lands in, but reads and drops on
a pipe; -n and -s take 0x hex; displayed offsets count from the start of the
stream. Unlike util-linux the last line of the default format is not padded
with trailing blanks.

`df [-h] [-k] [FILE...]` is one row, because there is one filesystem: size,
used and available in 1K blocks (or K/M/G rounded up with -h) and the
rounded-up use percentage. The numbers come from the same KINFO_DISK text that
`ls` ends on, so the two always agree. A FILE only has to exist.

The kernel is untouched. Tested in QEMU against Python reference dumps (both
formats, squeezing, -v, -n, -s, two files as one stream, a pipe, a pipe with -s,
missing files and bad numbers, a 300000-byte file in slices, and the whole file
through `tail` and `wc`), a 600000-byte `cat` join checked at the seam, bytes
passing through exactly, and df against `ls`, with the free space falling by a
copy's size and coming back when it is removed.
Host 244 + 82 + 163, QEMU 650.

## v0.24.5 "Handshake"

`find`, with the standard name and flags it can honestly offer.

`find [PATH...] [EXPRESSION]` visits each PATH (default `.`) and everything
under it, and evaluates the expression for every entry. Paths print as given
with the name added (`find docs` gives `docs/a.txt`, bare `find` gives
`./a.txt`), in the order the disk keeps them. The expression is parsed before
anything is touched, by a small recursive-descent parser with GNU's precedence:
`!` binds tightest, then `-a` (also implied between tests), then `-o`, with
`( )` or `\( \)` to group. If there is no action the whole expression is
and-ed with `-print`.

Tests: `-name` and `-iname` (glob with `* ? [a-z] [!x]`, and since the disk
ignores case both match without regard to it), `-path`, `-type f|d`,
`-size [+-]N[bckMG]` (rounded up like GNU, so `-size -1k` means empty),
`-empty`. Options: `-maxdepth`, `-mindepth`, `-depth`. Actions: `-print`,
`-print0`, `-exec CMD ... ;` with `{}` replaced anywhere in an argument, and
`-delete` (files and empty directories; implies `-depth`).

A directory is read in full into memory before any entry in it is visited, so
`-delete` and `-exec` may remove or create files without disturbing the walk.
`-exec` can only run programs: `rm`, `cat` and the other shell built-ins are
not programs, and the error says so, which is why `-delete` is there.
Known limits: no `-exec ... +`, no `-prune`, `-newer`, `-mtime` or `-perm`,
no `-type l`, and a path must fit in 255 characters, which the kernel enforces
anyway. Exit status is 0, 1 for an unreadable PATH or a failed delete, 2 for a
bad expression.

The kernel is untouched. Tested in QEMU against a Python model of a nine-entry
tree: every test and option alone and combined (case, classes, rounding of
sizes, depths, operators and both parenthesis spellings), several starting
points and a trailing slash, `-depth` order, `-exec` true and false and with a
built-in, every parse error with its exit status, a twelve-level chain, a
40-entry directory, and `-delete` on 40 files in one directory, on a whole
subtree, on a non-empty directory (refused) and on everything.
Host 244 + 82 + 163, QEMU 725.

## v0.24.6 "Handshake"

Move the 8: `ls`, `cat`, `echo`, `touch`, `rm`, `mkdir`, `rmdir` and `mv` are
now programs on the disk instead of Saklas built-ins, so they work after `|`,
read standard input, take `<`, and run under `find -exec`. The rule is the one
Linux follows: the shell keeps only what has to change its own state (`cd`,
`pwd`, `exit`, `history`, `color` and so on) and everything else is a program.
Each command exists once; nothing is both. `write`, which is not a standard
tool, stays a built-in.

Two helpers moved into the user library so every program shares them:
`name_ok` (the file-name check, so the "not a valid file name" message
survives) and `fs_error`. All eight accept `--`, take several operands where
that makes sense (`rm a b c`, `mkdir x y`, `mv a b dir`), and exit 0, 1 for a
failed operand, or 2 for bad usage.

Behaviour that changed, on purpose:

- A failing program makes Saklas print the dim `[exit status N]` line;
  built-in errors never did.
- Errors are plain and prefixed with the program name
  (`ls: nope: no such file or directory`).
- `echo` no longer keeps unquoted runs of blanks; quote the text to keep them.
  `write` still does.
- Unknown options are refused with a usage line and status 2 (`rm -r`,
  `mkdir -p`, `ls -l`) rather than being read as file names.
- `touch` on an existing file is still an error, as before. That is not what
  POSIX `touch` does, and it is kept only so this release moves things
  without changing them.
- `help` lists 22 commands instead of 30. Tab completion already treated
  programs in the root as commands, so nothing is lost there.

`find`'s hint for `-exec` now names `cd` and `pwd` as the commands that are
still built in. The kernel is untouched. A `--` bug found by the new tests
(a dash-leading name after `--` was still refused) is fixed in all eight.
The text-tool tests now build their 30-line file from `help` plus the first
eight lines again, since `help` got shorter.

New `coreutils_tests` (42 checks): the programs are on the disk, echo's flags
and quoting and pipes, cat on stdin and `-` and several files, every program's
multiple operands, errors and exit statuses, `--` with a dash-leading name,
rmdir of the current directory, `find -exec rm`, and cleanup.
Host 244 + 82 + 163, QEMU 771.

## v0.24.7 "Handshake"

Smoothing the rough edges left by the last few releases. The eight programs
that left the shell in v0.24.6 refused most options on purpose, to keep that
release a pure move; this one gives them the ones people actually reach for.

- `touch` no longer errors on a file that exists; it leaves it alone (there is
  still no call to set a time, so it keeps its old date). `-c` creates nothing.
- `mkdir -p` makes the parents and accepts an existing directory.
- `rm -f` is quiet about missing files, `rm -r` (or `-R`) takes whole trees.
  `.`, `..` and `/` are refused however they are spelled.
- `cp -r` copies directories (merging into one that exists), `-n` keeps an
  existing target, `-f` is accepted. A directory cannot be copied into itself:
  the check climbs from the destination through `..` comparing directory ids,
  so `cp -r . sub/z` and `cp -r /a /a/b/c` are caught however they are written.
  A directory's entries are read before its copy is made, so nothing copied
  into it is itself copied.
- `mv` replaces an existing file, as it does everywhere else, with `-n` to
  keep it and `-f` accepted. A directory is never replaced, nor a file by one,
  and a move onto itself says so. A name that differs only in case is a
  rename, not a clash, since the disk ignores case. Every check is made before
  the old target is deleted, but the delete and the rename are two steps.
- `ls` takes several paths and files (files first, then each directory under
  its name), `-R` goes down, and `-l -1 -a -A` are accepted and change
  nothing, since the listing is always the long form and the disk has no
  hidden files.
- `cat -n` numbers lines, continuing across files. `echo -e` reads
  `\n \t \r \a \b \e \\ \0NNN \xHH` and `\c`, `-E` is the default, and the
  flags bundle (`-ne`) the way they do elsewhere. echo now checks its writes.

In the user library: `getflags` splits bundled single-letter flags from
operands anywhere before `--` (all of the above use it), `read_dir` returns a
directory's entries whole, `join_path`, and `path_inside`.

Behaviour that changed from v0.24.6: `touch` on an existing file and `mv`
onto one used to be errors. Both are the usual behaviour now.

New `flags_tests`: 70 checks, one or more per option above, including a
directory of 50 entries (past the 32 at which the entry list first grows)
copied and removed whole, `rm -r` of the current directory, every spelling of
`.` and `..`, and a copy into itself from four directions. Three checks from
older releases that asserted the refusals were updated.
Host 244 + 82 + 163, QEMU 841.

## v0.24.8 "Handshake"

`grep` and `wc` were the two text tools still stuck at what the shell once
needed: grep did plain text and `-i`, cut lines at 1024 bytes and could not be
told anything; wc always printed three 7-wide columns.

- A regular-expression engine, `src/programs/lib/regex.c`: POSIX basic and
  extended syntax with the GNU extras (`\w \W \s \S \b \B \< \>`, `\+ \? \|`
  in basic), bracket expressions with ranges and `[:classes:]`, intervals,
  `-i`, `-w`, `-x`, `-F`. It compiles to a small instruction list and is run
  Pike-VM style, all threads side by side, so the time is the line length times
  the pattern size and nothing backtracks: `(a*)*b` on 5000 a's is instant.
  Matches are leftmost-longest, as POSIX says. No back-references (it says so
  rather than guessing). It copies GNU where GNU is odd and consistent: a
  leading `*` is a plain character in basic syntax and ignored in extended, a
  lone `)` is plain in extended, `-w` accepts an empty match between two
  non-word characters. It does not copy GNU where GNU contradicts itself
  (a `^`, `$` or `\<` in the middle of an extended pattern behaves differently
  for line selection and for `-o`).
- `grep [OPTION]... PATTERNS [FILE]...`: `-E -F -G -e -f -i -y -v -w -x -c -l
  -L -m -n -o -q -s -H -h -r -R -a -A -B -C -NUM`. Lines are any length. Output
  is flushed before grep waits for input, so it works mid-pipeline. Context
  groups get `--` between them, across files too; `-B` keeps a ring of the
  last N lines that is only as big as the lines held. A file with a NUL in it
  prints "Binary file X matches" unless `-a`. `-r` walks directories in name
  order (with no file, `.`, and bare names), and a directory without `-r` is an
  error. Exit 0 selected, 1 none, 2 error (but `-q` with a match is 0).
- `wc [-lwcmL]`: only the columns asked for, always in the order lines, words,
  bytes, longest line. White space is the C set (so `\v \f \r` count). A single
  number for a single file or for standard input is printed bare, as
  `$(wc -l < f)` wants; anything else keeps the 7-wide columns, with a total
  for several files (`-L` totals as the maximum).
- `ls` sorts by name, case ignored, in every directory and under `-R`; the
  disk's own order is the order the files were made in. `sort_dirents` is in
  the user library, and grep -r uses it.

Behaviour that changed: grep's pattern is a regular expression now, so
`grep a.c` means "a, anything, c" and `grep '('` is an error; use `-F` for the
old behaviour. `wc -l f` prints `4 f`, not three columns.

Testing: `tests/test_regex.c` runs 818 patterns against what GNU grep selects
and prints (`gen_regex_cases.py` makes the table with real grep, 700 of them
from a seeded generator, plus a hand-written set of syntax and error cases),
under the address and undefined-behaviour sanitizers, plus limits, patterns that
ruin a backtracker, 200 000-byte lines, and recompiling in a loop. Several
thousand more random patterns were compared by hand during development, which
is where the differences above came from.
New `grepwc_tests` in QEMU: 120 checks.
Host 244 + 82 + 163 + 2039 (regex), QEMU 961.

## v0.24.9 "Pipework"

The pipeline tools that sort, uniq, grep and wc had been waiting for.

- `cut -b|-c|-f LIST [-d C] [-s] [--complement] [--output-delimiter=S]`: lists
  like `1,3-5,8-` and `-3`; fields split on a tab or on `-d`; a line with no
  delimiter is passed through unless `-s`. Overlapping ranges merge, touching
  ones stay apart so `--output-delimiter` shows the seam, an empty output
  delimiter is a NUL byte. All as GNU does it.
- `tr [-cCdst] SET1 [SET2]`: ranges, the twelve `[:class:]`es, `[=c=]`, `[c*N]`
  and `[c*]` repeats, `\\ \a \b \f \n \r \t \v \NNN` escapes. A short SET2
  repeats its last byte, `-t` cuts SET1 instead, `-c` is the ascending
  complement, `-s` squeezes the translated bytes, `-ds` deletes then squeezes.
- `tee [-a] [FILE...]`: every chunk is written to the screen first, then the
  files, before waiting for more, so it works mid-pipeline. A file it cannot
  open is reported and skipped (exit 1) without starving the others; a file
  named `-` is a file, as in GNU.
- `src/programs/lib/lineio.[ch]`: a buffered writer (exits on a write error,
  quietly on EPIPE), a buffered reader that flushes the writer before it
  blocks, and a growing line type. The next text tools use it.

Testing: `tests/gen_text_cases.py` runs 231 command lines (about 130 for cut, 90
for tr, 11 for tee, in every form that bash and RingTail's shell both read)
through the real GNU tools and writes the outputs to `tests/text_cases.py`;
`gnu_text_tests` types them into RingTail and compares, in a fresh directory
each. Messages, which GNU words differently, are checked by hand: 16 checks.
QEMU 1208.

## v0.24.10 "Loose Change"

The small tools every pipeline and script reaches for.

- `basename` (`NAME [SUFFIX]`, `-a`, `-s`), `dirname`: trailing slashes, `//`,
  empty names and a suffix that is the whole name behave as in GNU.
- `seq [-w] [-s SEP] [FIRST [INCR]] LAST`: whole numbers and decimals (up to
  9 places, done in scaled 64-bit integers, so `seq 0 0.1 1` has no rounding
  noise). The places printed are those of the more precise of FIRST and INCR;
  `-w` pads to the width of FIRST or LAST as typed (`seq -w 007 009`), zeros
  after a sign.
- `cmp [-l] [-s] [-n N] FILE1 [FILE2]`: "a b differ: byte N, line M", `-l`
  lists every difference as decimal offset and octal values (offset column as
  wide as the shorter file's size, like GNU), "EOF on F after byte N" on
  stderr, statuses 0/1/2. GNU 3.7 and older say "char" where newer say "byte";
  RingTail says "byte".
- `yes`, `true`, `false`.

Testing: 185 more generated cases against GNU (basename, dirname, seq in
all its flag combinations, cmp) and 21 hand-written checks of messages and
statuses. Host 244 + 82 + 163 + 2039, QEMU 1414.

## v0.24.11 "Sedition"

`sed`, and the capture groups in the regex engine it needs.

- Regex: `RE_GROUPS` makes the Pike VM remember where each parenthesised group
  matched (`re_exec`, up to 31 groups). The overall match is still the
  leftmost-longest; where a group can match in several ways it takes the first
  way the pattern lists. `RE_MULTI` lets `^` and `$` match beside a newline
  inside the text (sed's `M`), and `RE_ONEPAT` stops a newline in the pattern
  from separating patterns, as grep does. Back-references inside a pattern are
  still refused.
- `sed [-nsEr] [-i[SUFFIX]] [-l N] [-e SCRIPT | -f FILE | SCRIPT] [FILE...]`,
  with GNU's long options for those. Addresses `N`, `$`, `/re/` (with `I` and
  `M`), `\cREc`, `first~step`, `0,/re/`, `a,+N`, `a,~N` and `!`; commands
  `{ } s y d D p P n N g G h H x z = l a i c r w b t T : q Q #`. `s` has `&`,
  `\0` to `\9`, `\n`, `\t`, the case changes `\U \L \u \l \E`, the
  flags `g p N I M w FILE`, any delimiter, and the empty pattern reuses the last
  regex used. `a`, `i` and `c` take GNU's one-line form (`a text`), `a\text`
  with its blanks kept, and the classic `a\` NEWLINE form; `w /dev/stdout`
  works. `-i` writes `NAME.sedtmp`, optionally renames the original to
  `NAME+SUFFIX`, deletes `NAME` and renames the temporary into its place (the
  disk's rename does not replace). Messages and exit statuses follow GNU's (1
  for a bad script, 2 for a missing input file, 4 for a missing label or no
  input files with `-i`).
- Quirks copied from GNU on purpose: `n` and `N` on the last line print and
  stop, `addr,~N` runs to the next multiple of N even from a multiple, `Q`
  drops what `a` and `r` queued, `c` in a range prints once at its end, `t`'s
  flag is cleared whenever a new line is read (also by `n` and `N`).
- Not done: back-references in patterns, `e`, `F`, `W`, `R`, `v`, `-z`,
  `--debug`, the `\c \d \o \x` escapes, `\` and `\'` anchors.

Testing: 391 more generated cases against GNU sed (every command, the
address forms, `s` flags, in-place editing, files without a final newline,
multiple files with and without `-s`), a table of 47 bad scripts whose exit
status is compared, 12 hand-written checks of messages, and 484 patterns with
groups whose captures are compared against GNU sed over the regex corpus (478
agree; the other 6 are random patterns with tied top-level branches where
glibc picks a later branch's groups, listed in test_regex.c). Host 244 + 82 +
163 + 21394, QEMU 1817.

## v0.24.12 "Wild Cards"

Saklas expands wildcards.

- An unquoted word containing `*`, `?` or a `[...]` set (with `!` or `^` to
  negate and `a-z` ranges) is replaced by every matching name, sorted and
  compared ignoring case (FAT does not care). Each `/` in the word is one
  directory level, so `*/x`, `d/*.txt` and `*/*` work. A leading dot has to be
  typed. A pattern that matches nothing stays as typed, as in bash; a quoted
  word is never expanded; a `[` with no closing bracket is a plain character.
- Too many matches for the room (64 words, 4 KB of names) print an error and
  run nothing, rather than a cut-off list.
- To make expansions useful the argument limits grew: ARGV_MAX 16 to 64,
  and the bytes of arguments a program can be started with to 3072 (loader and
  spawn syscall).
- Not done: `{a,b}`, `~`, `\` escapes, `**`, expansion in redirection targets.

Testing: about 60 generated cases against bash in temp directories (names are
all lowercase and start with `zq`, since the guest's root holds other files),
including quoting, dot files, sets, subdirectory patterns, no-match, and 20
matches passed on to `wc`. Host 244 + 82 + 163 + 21394, QEMU 1881.

## v0.24.13 "Spot the Difference"

diff, built to pick the same changes GNU diff does, not just a shortest set.

- `src/programs/lib/diffcore.c` is the analysis and the printers, with no
  system calls so the host can test it. It follows GNU's method step for step:
  the identical start and end are set aside (keeping as many lines of them as
  the context, so a change can still slide into them), lines that match
  nothing on the other side are put aside before the search, the Myers search
  finds middle snakes with GNU's tie-breaking and its cost cap for big files,
  and each change is slid to line up with its neighbours. Two diffs can both
  be minimal and look nothing alike, so this is what makes the output match.
- Normal, unified (`-u`, `-U N`) and context (`-c`, `-C N`) output, with
  `\ No newline at end of file`. `-i`, `-b`, `-w` compare lines loosely;
  `-d` searches without the shortcuts.
- `diff` itself: `-q`, `-s`, `-a`, `-N`, `-r`, `--label`, long options, `-`
  for standard input (read once, so `diff - -` works), a file against its
  namesake in a directory, and directory walks with GNU's messages (`Only in`,
  `Common subdirectories`, `File X is a regular file while file Y is a
  directory`, the `diff -r a/x b/x` line before each file). Headers carry the
  FAT write time (to two seconds, as `.000000000 +0000`), names with spaces or
  odd bytes are quoted the way GNU quotes them, and binary files (a NUL in the
  first 4 KB) are only reported. Several context lengths: the largest wins;
  mixing `-u` and `-c` is an error, as in GNU.
- Not done: `-y` side by side, `-e`/`-n` ed and RCS scripts, `-p`/`-F`
  function names, `-B`, `-I`, `-x`/`--exclude`, `--color`,
  `--strip-trailing-cr`, `-t`.
- README's limits said the network had no TCP, which stopped being true in
  v0.24.0; the resolver's error for a truncated answer said the same. Both now
  say what is true: the resolver does not retry over TCP.

Testing: tests/test_diff.c checks the core against 2024 cases from GNU diff
3.10 (random pairs, mostly a file and an edited copy drawn from a few short
lines so that many minimal answers tie, with lines that differ only in case or
blanks and files without a final newline), in every format and context size
and under -i, -b, -w and --minimal. All agree, byte for byte; so did 140
pairs of up to 60000 lines, checked once outside the suite, where GNU's cost
cap comes into play. In the guest: 55 generated cases against GNU diff
(formats, flags, standard input, empty files, binary files, directories with
-r, -q, -s, -N, labels, a quoted name) and 12 hand checks for headers, times
and messages. Host 244 + 82 + 163 + 21394 + 2024, QEMU 1948.

## v0.25.0 "Trust Me"

TLS 1.3, written from the RFCs with no borrowed code, and `fetch https://`.
The connection is encrypted; the server's certificate is not checked yet, so
it is not proven who is at the other end. That is the next release, and until
then fetch prints a warning on every HTTPS connection.

- Cryptography, all in plain C with no tables or branches that depend on
  secrets: SHA-256 with HMAC and HKDF and ChaCha20 (`src/lib`, since the
  kernel uses them too), Poly1305 and the two as an AEAD (`chachapoly.c`), and
  X25519 (`x25519.c`). `ct.h` has the constant-time compare and a wipe the
  compiler cannot remove.
- Random numbers (`src/kernel/random.c`, syscall 60 `random`): a pool stirred
  by every interrupt's cycle count and by RDRAND where there is one, hashed
  into a ChaCha20 key per request, the key replaced before the answer goes
  out. TCP sequence numbers, ports and the DHCP id now come from it in place
  of an xorshift generator seeded by the clock, and so does the resolver's id.
- `src/programs/lib/tls.c`: a TLS 1.3 client (RFC 8446) with no system calls
  in it. TLS_CHACHA20_POLY1305_SHA256 and X25519 only. Server name, the
  middlebox-compatibility ChangeCipherSpec, HelloRetryRequest with a cookie,
  an empty Certificate when the server asks for ours, key updates both ways,
  tickets ignored, record and message limits, close_notify. Every rule a
  server can break is met with the alert the RFC names, and an error is never
  worked around. Certificate and CertificateVerify are parsed for shape only;
  `tls_peer_verified()` returns 0.
- `fetch` takes `https://` (port 443), follows redirects from either scheme
  to the other, and warns on stderr that the certificate was not checked.
  A body with no stated length that ends without the server's close_notify is
  an error and is not kept, since that is what a connection cut by someone
  else looks like.
- `fetch` now speaks HTTP/1.1, because a lot of servers refuse 1.0 (the first
  real site tried answered 426 Upgrade Required): one request per connection
  with `Connection: close`, chunked bodies decoded (extensions and trailers
  skipped), 1xx interim responses passed over, 204 and 304 known to have no
  body, any other transfer encoding refused.
- Not done: certificate verification (X.509, RSA and ECDSA signatures, a root
  store, names and dates), AES-GCM suites, P-256 key exchange, resumption,
  early data, client certificates, ALPN.

Testing: `tests/test_crypto.c` runs 3242 checks: the RFCs' worked examples and
Project Wycheproof's vectors for X25519, ChaCha20-Poly1305, HMAC and HKDF.
`tests/gen_tls_cases.py` records 185 conversations and `tests/test_tls.c`
replays them (749 checks). 181 are against a server scripted separately from
the client out of RFC 8446: 33 where it stays within its rights, however
oddly, and 148 where it does not (119 during the handshake, 29 after it), each
with its expected result and alert written down first. 4 are real OpenSSL
handshakes (Python's `ssl` and `openssl s_server`, with key updates in both
directions). Each is replayed whole, a
byte at a time and in odd pieces, and a selection is cut at every length and
has a bit flipped in every byte. The generator's own key schedule is checked
against RFC 8448's worked HelloRetryRequest example. In the guest: fetch over
TLS against OpenSSL on the host (the same pages as over HTTP, 300000 bytes
compared, redirects across schemes), every HTTP/1.1 body shape over both, and
four servers that are not what fetch hoped for (TLS 1.2 only, plain HTTP,
nonsense, a hang-up). Outside the suite, from QEMU: example.com, Google,
Wikipedia and GitHub over HTTPS, through a gateway that terminates TLS with a
stack that is not OpenSSL. Host 244 + 82 + 163 + 21394 + 2024 + 3242 + 749,
QEMU 1995.

## v0.26.0 "Prove It"

The other half of TLS: the server's certificate is checked, and fetch does it
by default. An HTTPS connection now proves who is at the other end, as far as
Mozilla's roots and the machine's clock can say.

- `tls_set_verify()`: tls.c hands a verifier the name asked for, every
  certificate the server sent, and the CertificateVerify scheme, signature
  and signed content, and the verifier's answer decides the handshake. Each
  `TLS_E_CERT_*` sends the alert RFC 8446 names; anything unclear fails
  closed. tls.c itself knows nothing of X.509.
- `sha512.c`: SHA-384 and SHA-512.
- `sig.c`: a big-number library (Montgomery multiplication on 32-bit limbs,
  odd moduli to 4096 bits; only public values pass through it), RSA PKCS#1
  v1.5 and RSA-PSS (2048 to 4096 bits), ECDSA on P-256 and P-384 in Jacobian
  coordinates, and a SubjectPublicKeyInfo reader. SHA-1 and smaller RSA keys
  are refused.
- `x509.c`: a strict DER reader; certificates (version 3 only, unknown
  critical extensions refused); path building, depth first, from the leaf
  through any of the certificates sent, in any order, to a root; and on each
  path every signature, the dates of every certificate including the root,
  basicConstraints, path lengths (self-issued ones not counted), key usage,
  EKU serverAuth, and name constraints on DNS names, IP addresses and
  directory names. The name is matched against subjectAltName only, the
  common name never, a wildcard standing for one whole leftmost label and
  never for a public-looking suffix of one label. `x509_tls_verify` adds the
  CertificateVerify check with the leaf's key, the scheme matched to it.
- `/roots.pem`: Mozilla's 117 roots, made by `tools/mkroots.py` from the
  Mozilla directory of the ca-certificates package. Not from the merged
  system bundle: in this sandbox it also carries the egress proxy's own CAs,
  which would have quietly made every man in the middle here trusted.
- `fetch` verifies by default and says why when it will not go on (not
  signed by anyone we trust, the wrong name, expired, ...). `-k` connects
  anyway with the old warning; `--ca FILE` trusts the certificates in FILE
  in place of the roots.
- Found on the way: reading a directoryName passed the same DER cursor in
  and out, which emptied it, so every certificate with a directoryName in
  subjectAltName or name constraints was refused. One of Mozilla's roots
  (Izenpe) has one, which is how it showed up.
- Not done: revocation (CRL, OCSP), certificate transparency, policies,
  e-mail and URI name constraints (a constrained CA whose leaf has such a
  name is refused rather than half-checked).

Testing: `tests/test_sig.c`, 8076 checks: SHA-384/512 against OpenSSL, then
3303 signature cases from Wycheproof and OpenSSL, every good one checked
again with a byte of the signature or message changed. `tests/test_x509.c`,
1618 checks: 806 cases, 196 of them every server case of x509-limbo within
scope, 382 a sample of its BetterTLS name-constraint cases, and 228 our own
from `tests/gen_x509_ours.py` (keys it derives itself, RFC 6979 ECDSA), each
with the exact error expected; 28 answered unlike limbo on purpose, each with
its reason in the generator. A mutation sweep, every early return in x509.c
disabled in turn, found six gaps in the cases and two checks that could
never fire; the cases were added and the checks removed. Every root in
`disk/roots.pem` must read. `tests/test_tls.c` now replays 210
conversations, the generator checking with Python's `cryptography` that the
signed content it hands the verifier verifies under the server's key. In
the guest: the test makes its own PKI (a P-256 root, an RSA intermediate,
P-384 and RSA leaves) and fetch refuses it by default, accepts it with
`--ca`, refuses a wrong name and an expired certificate, takes an RSA-PSS
server, and runs every earlier TLS test with `--ca`. Host QEMU_HOST_COUNTS,
QEMU QEMU_COUNT.

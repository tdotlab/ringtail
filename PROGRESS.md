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

Next: scheduler / multitasking (the timer now makes a scheduling quantum possible)

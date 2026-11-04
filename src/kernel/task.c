// A task is a function with its own stack. The scheduler is not a
// separate thread, but it no longer waits for a task to be polite:
// every timer tick (100 Hz), timer_handler calls task_preempt(),
// which switches to the next ready task even if the current task
// never yielded. A task that loops forever without yielding can
// no longer freeze the shell; the shell always gets a slice.
// Why reusing switch_to from an interrupt handler works: switch_to
// only saves the callee-saved registers and a return address, and
// the interrupted task's interrupt frame (pusha registers plus the
// CPU's EFLAGS/CS/EIP, pushed by timer_isr) stays on its stack
// underneath the switch_to frame. When that task is switched back
// to, switch_to returns into task_preempt, which returns through
// timer_handler into timer_isr, which popa/irets back to exactly
// where the task was interrupted. Nothing about the interrupted
// task's registers is lost, because the interrupt frame was never
// unwound, only parked.
// Critical sections use cli/sti, not a lock counter. A switch only
// ever happens at a yield/preempt point, and both disable interrupts
// (cli) before picking the next task and re-enable them (sti) after
// the switch resumes, so the pick and the state changes are atomic
// with respect to the timer. The same trick protects task_create,
// task_kill, and task_cleanup: while interrupts are off, the timer
// cannot fire, so the timer handler cannot switch tasks in the
// middle of a list operation. A lock counter would be wrong here:
// a yield's lock would stay held across the switch by the outgoing
// task and only be released by the incoming task's resume, so the
// count would hover above zero forever and preemption would never
// fire. With cli/sti, each task's interrupt state is restored by
// its own resume path, so no count has to balance across tasks.
// Known v0.5.0 limits, stated plainly: the heap, RAMFS, and VGA
// output are NOT protected this way. They are safe for the tasks
// that exist (shell, counter, busy) only because the shell is the
// only task that touches the heap and RAMFS, and counter/busy only
// print and sleep or spin, so no two tasks ever touch the same
// mutable structure at once. A future task that allocates or edits
// files while the shell does the same would need the same cli/sti
// protection around those operations, or it could corrupt the free
// list or the file list. Prints are also no longer atomic: a task
// can now be preempted in the middle of a print, so counter output
// and shell output can interleave mid-line. That is not corruption
// (each character write is still a single VGA cell write), but it
// is visible, and a print lock is the obvious next hardening step.
// Side benefit of preemption: shell `sleep` (timer_sleep) no longer
// blocks other tasks. In v0.4.0 it did, because nothing switched
// during the sleep loop. Now the timer preempts the sleeping main
// task on every tick, so counter and busy keep running during a
// shell sleep, and the sleep still ends on time because ticks keep
// advancing.
// v0.5.1 addendum: the print lock lives in kernel.c (vga_lock,
// recursive spinlock, held around a whole counter line and around
// a whole shell command's output), and task_current_id below is
// its owner check. The heap and RAMFS locks are interrupt
// save/restore in heap.c and ramfs.c, not here, because those
// operations are short and never wait, so masking the timer for
// their duration is safe, while masking it across a print or a
// sleep would not be.

#include <stdint.h>

#define TASK_READY   0
#define TASK_RUNNING 1
#define TASK_BLOCKED 2
#define TASK_DEAD    3

#define TASK_STACK_SIZE 4096
#define MAX_SLEEP_SECONDS (0x7FFFFFFFu / 100)

typedef struct task {
    uint32_t esp;        // MUST be first: switch_to saves/loads [task]
    uint32_t stack;      // heap address of the stack allocation
    uint32_t kstack_top; // top of the kernel stack (TSS esp0 for this task)
    uint32_t pd;         // physical page directory; kernel PD for kernel tasks
    // v0.7.0: user_stack_top is a VIRTUAL address, not a heap
    // pointer. A user task's stack lives in its own address
    // space, mapped by its page directory, not in the kernel heap
    // (heap pages are supervisor-only now, so a ring 3 stack
    // there would fault on first use). 0 for kernel tasks.
    uint32_t user_stack_top;
    uint32_t wake_tick;  // for BLOCKED tasks: tick at which to wake
    int id;
    int state;
    // For user tasks, entry is a VIRTUAL address (the program's
    // entry point, 0x08000000 in this design), not a kernel
    // function pointer. user_entry loads it into the iret frame.
    void (*entry)(void*);
    void* arg;
    char name[32];
    struct task* next;
    // v0.7.2: per-task file descriptors, for program file syscalls.
    // These sit at the END of the struct on purpose, so the offsets
    // task.asm relies on (esp 0, pd 12, user_stack_top 16, entry
    // 32) do not shift. Eight slots, numbered 0-7, each holding a
    // filename (copied at open), a byte offset (advanced by read
    // and write, reset by reopen), and a used flag. Kernel tasks
    // never open files through syscalls, so their slots stay
    // unused; user tasks start with all slots free. There are no
    // permissions or modes in this slice: every open is
    // read/write, every program can open any file, because there
    // are no users yet to give permissions to.
    int fd_used[8];
    uint32_t fd_offsets[8];
    char fd_names[8][32];
} task_t;

extern void print(const char* s);
extern void print_char(char c);
extern void print_color(const char* s, unsigned char color);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern uint32_t timer_get_ticks(void);
extern void switch_to(task_t* old, task_t* new);
extern void tss_set_esp0(uint32_t esp0);
extern uint32_t paging_kernel_pd(void);
extern void paging_destroy_user_pd(uint32_t pd_pa);
extern void gfx_task_exited(int id); // syscall.c: restore text mode if this task owned graphics

static task_t main_task;       // task 0, the kernel main loop itself
static task_t* task_list = 0;  // head of the list, main_task first
// NOT static: user_entry in task.asm reads current_task to find
// the new task's entry point and user stack. No other asm needs it.
task_t* current_task = 0;
static int next_id = 1;

// v0.5.1: id of the currently running task, for the VGA print
// lock's owner check. Returns -1 before task_init runs (the
// banner prints before then, but nothing locks before then
// either, so -1 is never used as a real owner).
int task_current_id(void) {
    return current_task ? current_task->id : -1;
}

// v0.7.1: whether the currently running task is a user program.
// Kernel tasks have no user stack (user_stack_top is 0); user
// tasks have a virtual user stack top. Fault handlers use this
// to decide between killing the task and halting the OS: a user
// program's fault is its own problem, a kernel task's fault is
// an OS bug, and the two deserve different endings.
int task_current_is_user(void) {
    return current_task ? (current_task->user_stack_top != 0) : 0;
}

// v0.7.2: file descriptor helpers, for the program file syscalls
// in syscall.c. These live here, not there, because only this
// file sees the task_t definition; syscall.c calls them through
// extern declarations and never touches the fd arrays directly.
// All four operate on the CURRENT task, which during a syscall
// is still the calling program (the trap changed privilege, not
// the task pointer). A descriptor is a per-task slot number,
// 0-7, not a global handle: two programs can both hold fd 0 for
// different files, and neither sees the other's table.
int task_fd_open(const char* name) {
    if (!current_task) return -1;
    for (int i = 0; i < 8; i++) {
        if (!current_task->fd_used[i]) {
            current_task->fd_used[i] = 1;
            current_task->fd_offsets[i] = 0;
            int j = 0;
            while (name[j] && j < 31) {
                current_task->fd_names[i][j] = name[j];
                j++;
            }
            current_task->fd_names[i][j] = 0;
            return i;
        }
    }
    return -1; // all eight slots in use
}

int task_fd_close(int fd) {
    if (!current_task || fd < 0 || fd >= 8) return -1;
    if (!current_task->fd_used[fd]) return -1;
    current_task->fd_used[fd] = 0;
    current_task->fd_offsets[fd] = 0;
    current_task->fd_names[fd][0] = 0;
    return 0;
}

int task_fd_lookup(int fd, char* name_out, uint32_t* offset_out) {
    if (!current_task || fd < 0 || fd >= 8) return -1;
    if (!current_task->fd_used[fd]) return -1;
    for (int i = 0; i < 32; i++) name_out[i] = current_task->fd_names[fd][i];
    *offset_out = current_task->fd_offsets[fd];
    return 0;
}

int task_fd_advance(int fd, uint32_t new_offset) {
    if (!current_task || fd < 0 || fd >= 8) return -1;
    if (!current_task->fd_used[fd]) return -1;
    current_task->fd_offsets[fd] = new_offset;
    return 0;
}

static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print_char('0'); return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) print_char(buf[--i]);
}

extern uint8_t stack_top; // boot stack top, from boot.asm

void task_init(void) {
    main_task.esp = 0;
    main_task.stack = 0;
    main_task.kstack_top = (uint32_t)&stack_top;
    // task_init runs after paging_init in kernel_main, so the
    // kernel page directory exists by now. The main task runs
    // with it, like every kernel task.
    main_task.pd = paging_kernel_pd();
    main_task.user_stack_top = 0;
    main_task.wake_tick = 0;
    main_task.id = 0;
    main_task.state = TASK_RUNNING;
    main_task.entry = 0;
    main_task.arg = 0;
    main_task.name[0] = 's'; main_task.name[1] = 'h';
    main_task.name[2] = 'e'; main_task.name[3] = 'l';
    main_task.name[4] = 'l'; main_task.name[5] = 0;
    main_task.next = 0;
    for (int i = 0; i < 8; i++) {
        main_task.fd_used[i] = 0;
        main_task.fd_offsets[i] = 0;
        main_task.fd_names[i][0] = 0;
    }
    task_list = &main_task;
    current_task = &main_task;
    next_id = 1;
}

// First task in the list after `from`, wrapping around, that is
// ready to run. A blocked task whose wake tick has arrived becomes
// ready here. Dead and still-sleeping tasks are skipped. Returns 0
// if nothing but `from` itself is runnable.
static task_t* task_pick_next(task_t* from) {
    task_t* t = from->next ? from->next : task_list;
    task_t* start = t;
    while (1) {
        if (t->state == TASK_BLOCKED) {
            // Wrap-safe wake check: (ticks - wake) >= 0 in signed
            // arithmetic means wake has passed, even across the
            // 32-bit tick wraparound, for sleeps under 2^31 ticks.
            if ((int32_t)(timer_get_ticks() - t->wake_tick) >= 0)
                t->state = TASK_READY;
        }
        if (t->state == TASK_READY) return t;
        t = t->next ? t->next : task_list;
        if (t == start) break;
    }
    return 0;
}

void task_yield(void) {
    // cli first, sti last: the pick and the state changes must not
    // be interrupted by a timer tick that would pick a task too.
    // The sti runs on resume, in the resumed task, so each task
    // restores its own interrupt state (see the header comment).
    __asm__ volatile("cli");
    task_t* old = current_task;
    task_t* next = task_pick_next(old);
    if (!next || next == old) { __asm__ volatile("sti"); return; }
    if (old->state == TASK_RUNNING) old->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;
    switch_to(old, next);
    // Execution resumes here when this task is switched back to.
    // Set TSS esp0 BEFORE sti: esp0 is only used when a ring 3
    // task traps into ring 0, and this task is still in ring 0
    // here, so setting it now (interrupts still off) means it is
    // already correct by the time this task returns to ring 3.
    // `old` here is the resuming task, not the outgoing one: the
    // switcher set current_task to this task before switching,
    // and this task's own yield call saved `old` as itself.
    tss_set_esp0(old->kstack_top);
    __asm__ volatile("sti");
    current_task = old;
    old->state = TASK_RUNNING;
}

// Timer-driven switch, called from timer_handler on every tick.
// Identical in shape to task_yield, but it is the timer, not the
// task, that decided to switch, so the current task may be in the
// middle of anything (a loop, a sleep, a print) and does not have
// to cooperate. If the old task was already BLOCKED or DEAD when
// the tick arrived (it set its state and was preempted before its
// own yield call), its state is left alone, not forced to READY.
void task_preempt(void) {
    __asm__ volatile("cli");
    task_t* old = current_task;
    task_t* next = task_pick_next(old);
    if (!next || next == old) { __asm__ volatile("sti"); return; }
    if (old->state == TASK_RUNNING) old->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;
    switch_to(old, next);
    // Resumed when this task is switched back to: its interrupt
    // frame is still below this frame, and unwinding continues
    // through timer_handler into timer_isr's popa/iret. Set esp0
    // before that unwind reaches ring 3, as in task_yield.
    tss_set_esp0(old->kstack_top);
    __asm__ volatile("sti");
    current_task = old;
    old->state = TASK_RUNNING;
}

// Sleep `seconds` seconds, yielding to other tasks meanwhile.
// Returns 0 on success, -1 if the duration is too large (same bound
// as timer_sleep). For tasks only: the shell's sleep command still
// uses timer_sleep, which blocks the scheduler.
int task_sleep(uint32_t seconds) {
    if (seconds > MAX_SLEEP_SECONDS) return -1;
    if (seconds == 0) { task_yield(); return 0; }
    current_task->state = TASK_BLOCKED;
    current_task->wake_tick = timer_get_ticks() + seconds * 100;
    task_yield();
    return 0;
}

void task_exit(void) {
    current_task->state = TASK_DEAD;
    task_yield();
    // Never reached: a dead task is never picked again, so the
    // switch away from it never returns. If it ever did, the task
    // would loop here yielding forever rather than running dead
    // code, but the scheduler guarantees it cannot happen.
    while (1) task_yield();
}

// Free stacks and descriptors of dead tasks. Called from the main
// loop, where the current task is always the main task, so a dead
// task's stack is never the stack running this code.
void task_cleanup(void) {
    // cli/sti: the list walk and the frees must not be interrupted
    // by a timer switch that could pick a task mid-unlink. The heap
    // frees inside are also covered, since interrupts stay off for
    // the whole walk (they are short operations, and a pending
    // timer IRQ fires as soon as sti runs).
    __asm__ volatile("cli");
    task_t* prev = task_list;
    task_t* t = task_list->next;
    while (t) {
        task_t* nxt = t->next;
        if (t->state == TASK_DEAD) {
            int dead_id = t->id;
            prev->next = nxt;
            if (t->stack) kfree((void*)t->stack);
            // User tasks own a page directory: destroying it
            // frees the user page table and every program and
            // stack frame it maps. Kernel tasks share the kernel
            // directory, which destroy refuses to free.
            if (t->pd) paging_destroy_user_pd(t->pd);
            kfree(t);
            // v0.9.0: if the dead task owned graphics mode, this
            // restores the text screen it would have restored
            // itself with GFX_END, so a faulted or careless GUI
            // cannot leave the OS in graphics mode with no
            // program drawing and no visible shell. It runs
            // under cleanup's cli, which is fine: the restore is
            // port writes and memory copies, it needs no IRQs.
            gfx_task_exited(dead_id);
        } else {
            prev = t;
        }
        t = nxt;
    }
    __asm__ volatile("sti");
}

static void task_trampoline(void) {
    // Re-enable interrupts before running the entry function.
    // switch_to is always called with interrupts off (task_yield
    // and task_preempt both cli before switching), so a fresh task
    // would otherwise start with interrupts off. A task that yields
    // or sleeps restores them itself via the sti after its switch,
    // but a task that never yields (like busy) would spin forever
    // with the timer and keyboard masked, and the whole system
    // would freeze with no timer preemption possible. The sti here
    // is the one place that guarantees every new task starts with
    // interrupts on, so preemption can reach it.
    __asm__ volatile("sti");
    task_t* t = current_task;
    t->entry(t->arg);
    task_exit();
}

// Create a task running entry(arg) on a fresh 4096-byte heap stack.
// Returns the task id, or -1 on out of memory. The stack is faked
// to look like a task that just called switch_to (see task.asm),
// so the first switch to it "returns" into task_trampoline.
int task_create(const char* name, void (*entry)(void*), void* arg) {
    // cli/sti around the whole creation, including the heap
    // allocations: a timer switch mid-append could hand another
    // task a half-linked list. Single return point so every path
    // re-enables interrupts.
    __asm__ volatile("cli");
    int result = -1;
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (t) {
        uint32_t stack = (uint32_t)kmalloc(TASK_STACK_SIZE);
        if (stack) {
            uint32_t top = (stack + TASK_STACK_SIZE) & ~0xFu;
            uint32_t* sp = (uint32_t*)(top - 20);
            sp[0] = 0; // ebx
            sp[1] = 0; // esi
            sp[2] = 0; // edi
            sp[3] = 0; // ebp
            sp[4] = (uint32_t)task_trampoline;
            t->esp = (uint32_t)sp;
            t->stack = stack;
            t->kstack_top = top;
            t->pd = paging_kernel_pd();
            t->user_stack_top = 0;
            t->wake_tick = 0;
            t->id = next_id++;
            t->state = TASK_READY;
            t->entry = entry;
            t->arg = arg;
            int i = 0;
            while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
            t->name[i] = 0;
            t->next = 0;
            for (int j = 0; j < 8; j++) {
                t->fd_used[j] = 0;
                t->fd_offsets[j] = 0;
                t->fd_names[j][0] = 0;
            }
            task_t* tail = task_list;
            while (tail->next) tail = tail->next;
            tail->next = t;
            result = t->id;
        } else {
            kfree(t);
        }
    }
    __asm__ volatile("sti");
    return result;
}

extern void user_entry(void);

// Create a task that runs a loaded program in ring 3, in its own
// address space. The caller (the loader) has already built the
// page directory: pd is its physical address, entry_va is the
// program's virtual entry point, stack_top_va is the virtual top
// of its user stack. This function only builds the kernel side:
// a kernel stack, faked like task_create's, except its return
// address is user_entry, not task_trampoline, and user_entry
// irets into ring 3 at entry_va with ESP = stack_top_va. The
// program's pages and stack frame belong to the page directory
// and are freed by paging_destroy_user_pd when the task dies,
// so this function allocates no user memory itself. TSS esp0 is
// set here, before the first switch, because the first ring 3
// trap needs it already correct (see the v0.6.0 comment on
// task_create_user, which this replaces: programs no longer
// live in the kernel image, so there is no kernel function
// pointer to enter, only a virtual address). Returns the task
// id, or -1 on out of memory. Programs must exit via the exit
// syscall: user_entry has nowhere to return to if they return.
int task_create_user(const char* name, uint32_t pd_pa, uint32_t entry_va, uint32_t stack_top_va) {
    __asm__ volatile("cli");
    int result = -1;
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (t) {
        uint32_t stack = (uint32_t)kmalloc(TASK_STACK_SIZE);
        if (stack) {
            uint32_t top = (stack + TASK_STACK_SIZE) & ~0xFu;
            uint32_t* sp = (uint32_t*)(top - 20);
            sp[0] = 0; // ebx
            sp[1] = 0; // esi
            sp[2] = 0; // edi
            sp[3] = 0; // ebp
            sp[4] = (uint32_t)user_entry;
            t->esp = (uint32_t)sp;
            t->stack = stack;
            t->kstack_top = top;
            t->pd = pd_pa;
            t->user_stack_top = stack_top_va;
            t->wake_tick = 0;
            t->id = next_id++;
            t->state = TASK_READY;
            t->entry = (void (*)(void*))entry_va;
            t->arg = 0;
            int i = 0;
            while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
            t->name[i] = 0;
            t->next = 0;
            for (int j = 0; j < 8; j++) {
                t->fd_used[j] = 0;
                t->fd_offsets[j] = 0;
                t->fd_names[j][0] = 0;
            }
            task_t* tail = task_list;
            while (tail->next) tail = tail->next;
            tail->next = t;
            tss_set_esp0(top);
            result = t->id;
        } else {
            kfree(t);
        }
    }
    __asm__ volatile("sti");
    return result;
}

// Mark a task dead; the main loop's task_cleanup frees it. Cannot
// kill the main task (id 0) or a task that does not exist, and a
// task may not kill itself (it should return, which exits it).
int task_kill(int id) {
    // cli/sti: marking a task dead while the timer is picking the
    // next task could pick a task that is about to die, or miss
    // the kill entirely. Single return point, as in task_create.
    __asm__ volatile("cli");
    int result = -1;
    if (id != 0) {
        task_t* t = task_list->next;
        while (t) {
            if (t->id == id) {
                if (t != current_task) {
                    t->state = TASK_DEAD;
                    result = 0;
                }
                break;
            }
            t = t->next;
        }
    }
    __asm__ volatile("sti");
    return result;
}

static const char* task_state_name(int state) {
    if (state == TASK_READY) return "ready";
    if (state == TASK_RUNNING) return "running";
    if (state == TASK_BLOCKED) return "sleeping";
    return "dead";
}

void task_list_tasks(void) {
    task_t* t = task_list;
    while (t) {
        print_num((uint32_t)t->id);
        print("  ");
        print(t->name);
        print("  ");
        print(task_state_name(t->state));
        print("\n");
        t = t->next;
    }
}

// Demo task: counts once a second, forever, sleeping between
// counts. v0.5.0 change: its prints are NO LONGER atomic. A timer
// tick can preempt it in the middle of "counter: N", so its output
// can interleave mid-line with shell output. That is the visible
// price of preemption, and the reason a print lock is on the list
// for a later hardening pass (see the header comment).
extern void vga_lock(void);
extern void vga_unlock(void);

static void counter_task(void* arg) {
    (void)arg;
    uint32_t n = 0;
    while (1) {
        // v0.5.1: the whole line under one lock, so it can no
        // longer interleave mid-line with shell output. The lock
        // is released before sleeping, so the shell can print
        // while the counter sleeps.
        vga_lock();
        print("counter: ");
        print_num(n++);
        print("\n");
        vga_unlock();
        task_sleep(1);
    }
}

int task_spawn_counter(void) {
    return task_create("counter", counter_task, 0);
}

// Demo task: spins forever and never yields, never sleeps, never
// prints. Under cooperative scheduling this would freeze the whole
// OS, because no task would ever get a turn again. Under preemption
// it is harmless: the timer switches away from it every 10 ms, the
// shell stays responsive, and `kill` stops it. This is the task
// that proves preemption works. The pause instruction keeps the
// spin loop from being optimized away and is the polite way to
// spin on x86 (it hints to the CPU that this is a spin-wait).
static void busy_task(void* arg) {
    (void)arg;
    while (1) {
        __asm__ volatile("pause");
    }
}

int task_spawn_busy(void) {
    return task_create("busy", busy_task, 0);
}

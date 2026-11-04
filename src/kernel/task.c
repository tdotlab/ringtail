// Tasks, the scheduler, and the primitives everything else uses to
// wait: blocking on a channel, sleeping, joining, mutexes.
//
// Rules that keep this correct:
//  - schedule() runs with interrupts off, always. Every switch
//    happens inside it, so every task is switched out with IF=0 and
//    restores its own IF on the way back out of whatever called it.
//  - A task only ever frees its own resources while it can still
//    run (fds, display); its stack and address space are freed by
//    the scheduler later, from another task, once it is dead.
//  - Kills are requests. The target acts on them at a safe point.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "task.h"
#include "mm.h"
#include "tty.h"
#include "drivers.h"
#include "interrupt.h"
#include "ringtail/abi.h"

task_t* current;
static task_t* task_list_head;
static task_t* idle_task;
static int next_id;
static uint8_t fpu_clean[108];

#define STACK_CANARY 0x5AFEC0DEu

extern void switch_to(uint32_t* save_esp, uint32_t new_esp);
extern void enter_user(uint32_t eip, uint32_t esp);

const char* task_state_name(task_state_t s) {
    switch (s) {
        case TASK_READY: return "ready";
        case TASK_RUNNING: return "running";
        case TASK_SLEEPING: return "sleeping";
        case TASK_BLOCKED: return "blocked";
        case TASK_DEAD: return "dead";
    }
    return "?";
}

// The kernel itself never touches the FPU (it is compiled with
// general registers only), but programs may. Each task gets its own
// x87 state, saved and restored on every switch. That costs a few
// hundred cycles and avoids the lazy-switching machinery.
void fpu_init(void) {
    uint32_t cr0 = read_cr0();
    cr0 &= ~(1u << 2);  // EM: no emulation
    cr0 &= ~(1u << 3);  // TS: never trap on use
    cr0 |= (1u << 1);   // MP
    cr0 |= (1u << 5);   // NE: report x87 errors as exceptions
    write_cr0(cr0);
    __asm__ volatile("fninit; fnsave %0" : "=m"(fpu_clean));
}

void task_init(void) {
    // The boot context becomes the idle task. It runs only when
    // nothing else can, and never blocks.
    idle_task = kzalloc(sizeof(task_t));
    if (!idle_task) panic("no memory for the idle task");
    idle_task->id = next_id++;
    idle_task->state = TASK_RUNNING;
    idle_task->cr3 = kernel_pd();
    extern char stack_top[];
    idle_task->kstack_top = (uint32_t)stack_top;
    strlcpy(idle_task->name, "idle", TASK_NAME_MAX);
    memcpy(idle_task->fpu, fpu_clean, sizeof(fpu_clean));
    idle_task->next = idle_task;
    task_list_head = idle_task;
    current = idle_task;
}

task_t* task_find(int id) {
    task_t* t = task_list_head;
    do {
        if (t->id == id) return t;
        t = t->next;
    } while (t != task_list_head);
    return 0;
}

int task_current_id(void) { return current ? current->id : 0; }
int task_is_user(void) { return current && current->user; }

static void check_canary(task_t* t) {
    if (t->kstack && *(uint32_t*)t->kstack != STACK_CANARY)
        panic("kernel stack overflow in task %d (%s)", t->id, t->name);
}

// Exit statuses of recently reaped tasks, so a join that arrives
// after the task already exited and was reaped still gets its status.
// Ids are never reused, so an entry cannot be mistaken for another task.
#define EXITED_CACHE 64
static struct { int id, status; } exited[EXITED_CACHE];
static int exited_pos;

static void reap(task_t* t) {
    exited[exited_pos].id = t->id;
    exited[exited_pos].status = t->exit_status;
    exited_pos = (exited_pos + 1) % EXITED_CACHE;
    // Unlink. The list is circular and the idle task is never reaped,
    // so it always has a predecessor.
    task_t* p = task_list_head;
    while (p->next != t) p = p->next;
    p->next = t->next;
    if (t->cr3 != kernel_pd()) as_destroy(t->cr3);
    kfree(t->kstack);
    kfree(t);
}

void schedule(void) {
    task_t* prev = current;
    uint32_t now = timer_ticks();

    // Wake sleepers whose time has come, and reap the dead that
    // nobody is waiting on. prev cannot be reaped: its stack is the
    // one we are standing on.
    task_t* t = task_list_head->next;
    while (t != task_list_head) {
        task_t* nx = t->next;
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_tick) >= 0) t->state = TASK_READY;
        if (t->state == TASK_DEAD && t != prev && t->joiners == 0) reap(t);
        t = nx;
    }

    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;

    // Round robin, starting after prev; the idle task only if nobody
    // else is ready.
    task_t* next = 0;
    t = prev->next;
    for (;;) {
        if (t != idle_task && t->state == TASK_READY) { next = t; break; }
        if (t == prev) break;
        t = t->next;
    }
    if (!next) next = idle_task;
    next->state = TASK_RUNNING;
    if (next == prev) return;

    check_canary(prev);
    tss_set_esp0(next->kstack_top);
    if (next->cr3 != prev->cr3) write_cr3(next->cr3);
    __asm__ volatile("fnsave %0" : "=m"(prev->fpu));
    __asm__ volatile("frstor %0" :: "m"(next->fpu));
    current = next;
    switch_to(&prev->esp, next->esp);
}

void task_yield(void) {
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}

void task_block(void* chan) {
    current->state = TASK_BLOCKED;
    current->wait_chan = chan;
    schedule();
    current->wait_chan = 0;
}

void task_wakeup(void* chan) {
    uint32_t f = irq_save();
    task_t* t = task_list_head;
    do {
        if (t->state == TASK_BLOCKED && t->wait_chan == chan) {
            t->state = TASK_READY;
            need_resched = 1;
        }
        t = t->next;
    } while (t != task_list_head);
    irq_restore(f);
}

int task_sleep_ticks(uint32_t ticks) {
    uint32_t f = irq_save();
    uint32_t until = timer_ticks() + ticks;
    int r = 0;
    while ((int32_t)(timer_ticks() - until) < 0) {
        if (current->killed) { r = -EINTR; break; }
        current->wake_tick = until;
        current->state = TASK_SLEEPING;
        schedule();
    }
    irq_restore(f);
    return r;
}

// First code a new task runs, entered from switch_to's ret with
// interrupts still off.
static void task_trampoline(void) {
    task_t* t = current;
    if (t->user) {
        enter_user(t->user_eip, t->user_esp);  // does not return
    }
    sti();
    t->entry(t->arg);
    task_exit(0);
}

static task_t* task_alloc(const char* name) {
    task_t* t = kzalloc(sizeof(task_t));
    if (!t) return 0;
    t->kstack = kmalloc(KSTACK_SIZE);
    if (!t->kstack) { kfree(t); return 0; }
    *(uint32_t*)t->kstack = STACK_CANARY;
    t->kstack_top = (uint32_t)t->kstack + KSTACK_SIZE;
    // Fake the frame switch_to pops: ebx, esi, edi, ebp, return.
    uint32_t* sp = (uint32_t*)t->kstack_top;
    *--sp = (uint32_t)task_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    t->esp = (uint32_t)sp;
    t->cr3 = kernel_pd();
    t->parent = task_current_id();
    strlcpy(t->name, name, TASK_NAME_MAX);
    memcpy(t->fpu, fpu_clean, sizeof(fpu_clean));
    return t;
}

static int task_start(task_t* t) {
    uint32_t f = irq_save();
    t->id = next_id++;
    if (!t->job) t->job = t->id;
    t->state = TASK_READY;
    t->next = task_list_head->next;
    task_list_head->next = t;
    irq_restore(f);
    return t->id;
}

int task_create_kernel(const char* name, void (*entry)(void*), void* arg) {
    task_t* t = task_alloc(name);
    if (!t) return -ENOMEM;
    t->entry = entry;
    t->arg = arg;
    return task_start(t);  // a kernel task is its own job
}

int task_create_user(const char* name, uint32_t pd, uint32_t eip, uint32_t esp,
                     uint32_t brk, int job) {
    task_t* t = task_alloc(name);
    if (!t) { as_destroy(pd); return -ENOMEM; }
    t->user = 1;
    t->cr3 = pd;
    t->user_eip = eip;
    t->user_esp = esp;
    t->brk_start = t->brk = brk;
    t->job = job;
    files_init_std(t);
    return task_start(t);
}

void task_exit(int status) {
    task_t* t = current;
    if (t == idle_task) panic("idle task exited");
    // Release what only this task can release cleanly, while it can
    // still block: open files (which take the filesystem lock) and
    // the display.
    files_close_all(t);
    gfx_task_exit(t->id);
    cli();
    t->exit_status = status;
    t->state = TASK_DEAD;
    task_wakeup(t);           // joiners
    task_wakeup(&next_id);    // job waiters
    schedule();
    panic("dead task %d was scheduled", t->id);
}

int task_kill(int id) {
    uint32_t f = irq_save();
    task_t* t = task_find(id);
    int r = 0;
    if (!t || t->state == TASK_DEAD) r = -ESRCH;
    else if (t == idle_task) r = -EPERM;
    else {
        t->killed = 1;
        if (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING) t->state = TASK_READY;
        need_resched = 1;
    }
    irq_restore(f);
    return r;
}

void task_kill_job(int job) {
    if (job <= 0) return;
    uint32_t f = irq_save();
    task_t* t = task_list_head;
    do {
        if (t->job == job && t->state != TASK_DEAD && t != idle_task) task_kill(t->id);
        t = t->next;
    } while (t != task_list_head);
    irq_restore(f);
}

int task_join(int id) {
    uint32_t f = irq_save();
    task_t* t = task_find(id);
    int status = -ESRCH;
    if (!t) {
        for (int i = 0; i < EXITED_CACHE; i++)
            if (id > 0 && exited[i].id == id) status = exited[i].status;
    } else if (t != current && t != idle_task) {
        t->joiners++;
        while (t->state != TASK_DEAD && !current->killed) task_block(t);
        status = t->state == TASK_DEAD ? t->exit_status : -EINTR;
        t->joiners--;
    }
    irq_restore(f);
    return status;
}

int task_job_alive(int job) {
    uint32_t f = irq_save();
    int alive = 0;
    task_t* t = task_list_head;
    do {
        if (t->job == job && t->state != TASK_DEAD && t != idle_task) alive = 1;
        t = t->next;
    } while (t != task_list_head);
    irq_restore(f);
    return alive;
}

void task_wait_job(int job) {
    uint32_t f = irq_save();
    while (task_job_alive(job) && !current->killed) task_block(&next_id);
    irq_restore(f);
}

void task_list(void) {
    // Snapshot under cli, print afterwards: printing may block.
    struct { int id, job, user; task_state_t state; uint32_t pages; char name[TASK_NAME_MAX]; } snap[32];
    int n = 0;
    uint32_t f = irq_save();
    task_t* t = task_list_head;
    do {
        if (n < 32) {
            snap[n].id = t->id;
            snap[n].job = t->job;
            snap[n].user = t->user;
            snap[n].state = t->state;
            snap[n].pages = t->user ? as_pages(t->cr3) : 0;
            strlcpy(snap[n].name, t->name, TASK_NAME_MAX);
            n++;
        }
        t = t->next;
    } while (t != task_list_head);
    irq_restore(f);
    kprintf("  ID  JOB  KIND    STATE     MEM   NAME\n");
    for (int i = 0; i < n; i++) {
        char mem[12] = "-";
        if (snap[i].user) snprintf(mem, sizeof(mem), "%uK", snap[i].pages * 4);
        kprintf("%4d %4d  %-6s  %-8s %5s   %s\n", snap[i].id, snap[i].job,
                snap[i].user ? "user" : "kernel", task_state_name(snap[i].state), mem, snap[i].name);
    }
}

// ---- Mutexes ----

void mutex_lock(mutex_t* m) {
    uint32_t f = irq_save();
    if (m->locked && m->owner == current) {
        m->depth++;
    } else {
        while (m->locked) task_block(m);
        m->locked = 1;
        m->owner = current;
        m->depth = 1;
    }
    irq_restore(f);
}

void mutex_unlock(mutex_t* m) {
    uint32_t f = irq_save();
    if (m->owner != current) panic("mutex unlocked by a task that does not own it");
    if (--m->depth == 0) {
        m->locked = 0;
        m->owner = 0;
        task_wakeup(m);
    }
    irq_restore(f);
}

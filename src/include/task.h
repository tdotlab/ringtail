// Tasks and the scheduler. A task is a kernel thread with its own
// kernel stack; a user task also has its own address space and
// spends most of its time in ring 3.
#ifndef RINGTAIL_TASK_H
#define RINGTAIL_TASK_H

#include <stdint.h>
#include "fs.h"
#include "sync.h"
#include "lib.h"
#include "ringtail/abi.h"
#include "arch/fpu.h"
#include "arch/percpu.h"

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,  // waiting for a tick count
    TASK_BLOCKED,   // waiting on a channel
    TASK_DEAD,      // exited, waiting to be reaped
} task_state_t;

#define TASK_NAME_MAX 32
#define KSTACK_SIZE   (16 * 1024)

typedef struct task {
    uintptr_t ksp;         // saved kernel stack pointer while switched out
    uintptr_t cr3;         // top-level page table
    uint8_t* kstack;
    uintptr_t kstack_top;

    int id;
    int job;               // tasks started together share a job; Ctrl+C signals a job
    int parent;
    task_state_t state;
    int user;
    int is_idle;           // a CPU's idle task: never in the run queue, never exits
    // Signals (signal.c), a bit each. Anyone sets bits in sig_pending,
    // atomically; only the task itself clears them, or changes its
    // mask and actions.
    volatile uint32_t sig_pending;  // sent, not yet acted on
    uint32_t sig_blocked;           // held back; never SIGKILL
    sigaction_t sig_action[NSIG];
    int exit_status;
    int joiners;           // tasks waiting in task_join; keeps the corpse around
    uint32_t wake_tick;    // for TASK_SLEEPING, and TASK_BLOCKED when timed
    void* wait_chan;
    int timed;             // blocked with a deadline (task_block_timeout)

    void (*entry)(void*);
    void* arg;
    uintptr_t user_ip, user_sp;
    uintptr_t brk_start, brk;  // user heap: from the end of the image to the break
    struct shm_map { struct shm* obj; uintptr_t va; } shm[SHM_PER_TASK];  // obj 0: a free slot

    char name[TASK_NAME_MAX];
    ofile_t* fds[MAX_FDS];  // 0 where closed
    int cwd;               // current directory, a diskfs node (0 is the root)
    struct tty* tty;       // the controlling terminal; 0 is the console (tty.h)
    fpu_state_t fpu;       // floating-point registers, saved on every switch

    struct task* next;
} task_t;

// The running task, on this CPU (arch/percpu.h).
#define current ((task_t*)this_cpu_task())

void task_init(void);
// Another CPU's idle task, which its first code becomes (smp.c), and
// how it starts being one: on that CPU, with interrupts off.
task_t* task_idle_create(int cpu, uintptr_t stack_top);
void task_idle_enter(task_t* idle);
void fpu_init(void);
// Lay out a new task's kernel stack, which ends at top, so switch_to
// pops a zeroed frame and returns into entry. Returns the saved stack
// pointer (arch/<arch>/cpu.c).
uintptr_t arch_task_stack(uintptr_t top, void (*entry)(void));
int task_create_kernel(const char* name, void (*entry)(void*), void* arg);
// Takes ownership of pd. The task starts in ring 3 at ip with the
// given user stack pointer; its heap starts empty at brk. Job 0 makes
// it the leader of a new job. With std (three of the current task's
// descriptors, already checked), descriptors 0-2 are copies of those;
// without, they are the console. A held task does not run until
// task_release: that gives its creator time to hand it the terminal,
// which it would otherwise race to read.
int task_create_user(const char* name, uintptr_t pd, uintptr_t ip, uintptr_t sp,
                     uintptr_t brk, int job, const int std[SPAWN_FDS], int held);
void task_release(int id);
void task_exit(int status) __attribute__((noreturn));

// Must be called with interrupts disabled.
void schedule(void);
void task_yield(void);

// Waiting. Both must be called with interrupts disabled, after
// checking the condition being waited for, so a wakeup cannot slip
// in between the check and the sleep. Wakeups may be spurious:
// callers re-check their condition in a loop.
void task_block(void* chan);
// The same, but the task also wakes once the tick count reaches until.
void task_block_timeout(void* chan, uint32_t until);
void task_wakeup(void* chan);   // safe from interrupt handlers

// Sleep for a number of timer ticks. Returns 0, or -EINTR if the
// task was killed while sleeping.
int task_sleep_ticks(uint32_t ticks);

// Signals (signal.c). Sending is deferred: the target acts on it at its
// next safe point, on the way back to user mode (a kernel task has no
// handlers: a signal only makes its waits give up). A target blocked
// or asleep wakes, unless the signal is blocked. An ignored signal is
// dropped. Sending signal 0 only checks that the task is there.
int task_signal(int id, int sig);
int task_signal_user(int id, int sig);  // for programs: -EPERM for kernel tasks
void task_signal_job(int job, int sig);
struct tty;
void task_signal_tty(struct tty* tty, int sig);  // every task it is the terminal of
// A signal the task must act on is pending: its blocking waits give up
// with -EINTR.
static inline int task_signalled(const task_t* t) { return (t->sig_pending & ~t->sig_blocked) != 0; }
// Wait for a task to exit and return its exit status.
int task_join(int id);
// Wait until no live task belongs to the job.
void task_wait_job(int job);
int task_job_alive(int job);

task_t* task_find(int id);
int task_current_id(void);
int task_is_user(void);
// The task table as text, for the tasks command.
void task_format(sbuf_t* sb);
// 1 if the job exists (or its leader ended recently) and the current
// task may add tasks to it or hand it the terminal: its own job, or
// one whose leader it started.
int task_job_mine(int job);
const char* task_state_name(task_state_t s);

#endif

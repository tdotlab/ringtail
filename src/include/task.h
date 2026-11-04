// Tasks and the scheduler. A task is a kernel thread with its own
// kernel stack; a user task also has its own address space and
// spends most of its time in ring 3.
#ifndef RINGTAIL_TASK_H
#define RINGTAIL_TASK_H

#include <stdint.h>
#include "fs.h"
#include "sync.h"

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,  // waiting for a tick count
    TASK_BLOCKED,   // waiting on a channel
    TASK_DEAD,      // exited, waiting to be reaped
} task_state_t;

#define TASK_NAME_MAX 32
#define KSTACK_SIZE   (16 * 1024)

// Exit status of a task that was killed (Ctrl+C, kill) or that
// faulted. Normal exits use whatever the program returned.
#define EXIT_KILLED  (-1)
#define EXIT_FAULTED (-2)

typedef struct task {
    uint32_t esp;          // saved kernel stack pointer while switched out
    uint32_t cr3;          // page directory
    uint8_t* kstack;
    uint32_t kstack_top;

    int id;
    int job;               // tasks started together share a job; Ctrl+C kills a job
    int parent;
    task_state_t state;
    int user;
    volatile int killed;
    int exit_status;
    int joiners;           // tasks waiting in task_join; keeps the corpse around
    uint32_t wake_tick;
    void* wait_chan;

    void (*entry)(void*);
    void* arg;
    uint32_t user_eip, user_esp;

    char name[TASK_NAME_MAX];
    fd_t fds[MAX_FDS];
    uint8_t fpu[108];      // x87 state, fnsave format

    struct task* next;
} task_t;

extern task_t* current;

void task_init(void);
void fpu_init(void);
int task_create_kernel(const char* name, void (*entry)(void*), void* arg);
// Takes ownership of pd. The task starts in ring 3 at eip with the
// given user stack pointer.
int task_create_user(const char* name, uint32_t pd, uint32_t eip, uint32_t esp, int job);
void task_exit(int status) __attribute__((noreturn));

// Must be called with interrupts disabled.
void schedule(void);
void task_yield(void);

// Waiting. Both must be called with interrupts disabled, after
// checking the condition being waited for, so a wakeup cannot slip
// in between the check and the sleep. Wakeups may be spurious:
// callers re-check their condition in a loop.
void task_block(void* chan);
void task_wakeup(void* chan);   // safe from interrupt handlers

// Sleep for a number of timer ticks. Returns 0, or -EINTR if the
// task was killed while sleeping.
int task_sleep_ticks(uint32_t ticks);

// Deferred kill: the target stops at its next safe point (on the
// way back to user mode, or at a kernel task's own check).
int task_kill(int id);
void task_kill_job(int job);
// Wait for a task to exit and return its exit status.
int task_join(int id);
// Wait until no live task belongs to the job.
void task_wait_job(int job);
int task_job_alive(int job);

task_t* task_find(int id);
int task_current_id(void);
int task_is_user(void);
void task_list(void);
const char* task_state_name(task_state_t s);

#endif

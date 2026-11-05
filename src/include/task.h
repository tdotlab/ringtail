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
    uint32_t brk_start, brk;  // user heap: from the end of the image to the break

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
// given user stack pointer; its heap starts empty at brk. Job 0 makes
// it the leader of a new job. With std (three of the current task's
// descriptors, already checked), descriptors 0-2 are copies of those;
// without, they are the console.
int task_create_user(const char* name, uint32_t pd, uint32_t eip, uint32_t esp,
                     uint32_t brk, int job, const int std[3]);
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
int task_kill_user(int id);  // for programs: -EPERM for kernel tasks
void task_kill_job(int job);
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

// What each CPU keeps for itself, x86_64.
//
// In the kernel, gs's base is the running CPU's block (the GS_BASE
// MSR, set by gdt_init_cpu); in ring 3 it is the program's, which is
// always 0. Every way in from ring 3 does a swapgs to get the kernel's
// and every way out does one to put the program's back (isr.asm,
// switch.asm), so a field of the block is one gs-relative load, and
// nothing needs to know which CPU it is on to find it.
//
// The running task is read that way (current, task.h): in one
// instruction, so preemption cannot fall between finding the CPU and
// reading its task, which after a move to another CPU would be some
// other task. Anything else here belongs to the CPU, not the task, so
// this_cpu() is for code that runs with interrupts off: with them on,
// the task can be moved between reading the pointer and using it.
#ifndef RINGTAIL_ARCH_PERCPU_H
#define RINGTAIL_ARCH_PERCPU_H

#include <stdint.h>

struct task;

typedef struct cpu {
    uint64_t user_rsp;          // the SYSCALL entry parks the user's rsp here
    uint64_t kstack;            // the running task's kernel stack, for the SYSCALL entry
    struct cpu* self;           // where this block is, for this_cpu()
    struct task* task;          // the running task
    struct task* idle;          // this CPU's idle task
    int index;                  // 0 for the boot CPU, then in the order they started
    volatile int resched;       // switch tasks on the way out of this interrupt
    uint32_t apic_id;
    volatile int kicked;        // sent a reschedule IPI it has not acted on yet
    uint64_t ticks;             // timer interrupts this CPU has taken
    uint64_t switches;          // times it has switched to another task
} cpu_t;

// isr.asm and the accessors below know these.
#define CPU_SELF_OFFSET 16
#define CPU_TASK_OFFSET 24
_Static_assert(__builtin_offsetof(cpu_t, user_rsp) == 0 && __builtin_offsetof(cpu_t, kstack) == 8 &&
               __builtin_offsetof(cpu_t, self) == CPU_SELF_OFFSET &&
               __builtin_offsetof(cpu_t, task) == CPU_TASK_OFFSET, "isr.asm knows these offsets");

static inline cpu_t* this_cpu(void) {
    cpu_t* c;
    __asm__ volatile("mov %%gs:16, %0" : "=r"(c));
    return c;
}

static inline struct task* this_cpu_task(void) {
    struct task* t;
    __asm__ volatile("mov %%gs:24, %0" : "=r"(t));
    return t;
}

// The block of CPU index (0 to MAX_CPUS - 1), started or not (gdt.c).
cpu_t* cpu_get(int index);

#endif

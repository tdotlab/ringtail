// Signals: what a task does with the ones sent to it (task_signal in
// task.h sends them). The numbers, actions and system calls are in
// ringtail/abi.h.
#ifndef RINGTAIL_SIGNAL_H
#define RINGTAIL_SIGNAL_H

#include "arch/regs.h"

// On the way back to user mode with a signal pending and not blocked
// (interrupt_dispatch), interrupts on: each such signal is dropped if
// ignored, ends the task if left at its default, or gets its handler
// called, frames stacked on the program's own stack in that order.
void signal_deliver(regs_t* r);
// A fault in user mode that sends sig: 1 if the program catches it and
// its handler is now set to run, 0 if the task must end (the caller
// says why first, then calls signal_exit).
int signal_fault(regs_t* r, int sig);
// End the current task as killed by sig.
void signal_exit(int sig) __attribute__((noreturn));
// SYS_SIGRETURN, which needs the whole frame: back to where the program
// was when the handler was called.
void signal_return(regs_t* r);

// The architecture's part (arch/<arch>/sigframe.c). push makes r call
// handler(sig) on the user stack, returning to restorer, with a frame
// that saves r, the FPU and mask: 0 if the frame does not fit there.
// pop undoes it, from the frame at the user rsp of a SYS_SIGRETURN
// (0 if it cannot be read or says to go somewhere it may not).
int arch_signal_push(regs_t* r, uintptr_t handler, uintptr_t restorer, int sig, uint32_t mask);
int arch_signal_pop(regs_t* r, uint32_t* mask);

long sys_sigaction(uintptr_t sig, uintptr_t act, uintptr_t old);
long sys_sigmask(uintptr_t how, uintptr_t mask, uintptr_t unused);

#endif

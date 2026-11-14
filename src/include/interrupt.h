// Interrupt and exception entry. Every vector enters through the
// common stub in arch/<arch>/isr.asm, which builds a regs_t on the
// kernel stack.
#ifndef RINGTAIL_INTERRUPT_H
#define RINGTAIL_INTERRUPT_H

#include <stdint.h>

// regs_t, the frame, with regs_from_user, regs_ip, regs_fp and the
// SYSCALL_* accessors for the system call registers.
#include "arch/regs.h"

#define IRQ_BASE      32
#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_SERIAL1   4
#define IRQ_MOUSE     12
#define SYSCALL_VECTOR 0x80

typedef void (*irq_handler_t)(void);

void idt_init(void);
// Install a handler and unmask the line. Handlers run with interrupts
// off and must not block; the dispatcher sends the EOI.
void irq_register(int irq, irq_handler_t handler);

// Set by anything that makes another task runnable; the dispatcher
// reschedules on the way out of the interrupt.
extern volatile int need_resched;

void interrupt_dispatch(regs_t* r);

// The machine's part (arch/<arch>/cpu.c): a gate that runs handler in
// ring 0 with interrupts off, which code at privilege dpl may call
// with int; loading the table; and printing a frame.
void idt_set_gate(int vec, uintptr_t handler, int dpl);
void idt_install(void);
void dump_regs(const regs_t* r);

#endif

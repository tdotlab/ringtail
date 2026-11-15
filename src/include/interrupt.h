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
// Install a handler for vector IRQ_BASE + irq and unmask nothing: for
// an interrupt raised on that vector by something other than the
// board's line (the local APIC's timer, standing in for the PIT).
void irq_set_handler(int irq, irq_handler_t handler);
// Whether the line is asking for an interrupt not yet taken (with
// interrupts off).
int irq_pending(int irq);
// Moves from the 8259s to the APICs if ACPI describes them. After
// paging_init, which the APICs' registers are mapped with; idt_init
// set up the 8259s, which stay in use otherwise.
void irq_controller_init(void);
int irq_uses_apic(void);
// "local and I/O APIC", or "8259 PIC" and why.
const char* irq_controller(char* buf, uint32_t size);

// Set by anything that makes another task runnable; the dispatcher
// reschedules on the way out of the interrupt.
extern volatile int need_resched;

void interrupt_dispatch(regs_t* r);

// The machine's part (arch/<arch>/cpu.c): a gate that runs handler in
// ring 0 with interrupts off, which code at privilege dpl may call
// with int; loading the table (on the boot CPU, then on each of the
// others: they share it); and printing a frame.
void idt_set_gate(int vec, uintptr_t handler, int dpl);
void idt_install(void);
void idt_load_here(void);
void dump_regs(const regs_t* r);

#endif

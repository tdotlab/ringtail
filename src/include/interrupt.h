// Interrupt and exception entry. Every vector enters through the
// common stub in isr.asm, which builds a regs_t on the kernel stack.
#ifndef RINGTAIL_INTERRUPT_H
#define RINGTAIL_INTERRUPT_H

#include <stdint.h>

typedef struct regs {
    uint32_t gs, fs, es, ds;                         // pushed by the stub
    uint32_t edi, esi, ebp, esp0, ebx, edx, ecx, eax; // pusha
    uint32_t vector, err;                            // stub, or CPU error code
    uint32_t eip, cs, eflags;                        // CPU
    uint32_t user_esp, user_ss;                      // CPU, only when coming from ring 3
} regs_t;

static inline int regs_from_user(const regs_t* r) { return (r->cs & 3) == 3; }

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

#endif

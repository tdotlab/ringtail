// The i386 parts of interrupt and task handling: IDT gates, the
// register dump, the stack a new task starts on, and the FPU.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "console.h"
#include "task.h"

typedef struct {
    uint16_t base_low;
    uint16_t sel;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;

extern void idt_load(idt_ptr_t* p);

void idt_set_gate(int vec, uintptr_t handler, int dpl) {
    idt[vec].base_low = handler & 0xFFFF;
    idt[vec].base_high = (handler >> 16) & 0xFFFF;
    idt[vec].sel = SEL_KERNEL_CODE;
    idt[vec].zero = 0;
    idt[vec].flags = (uint8_t)(0x8E | dpl << 5);  // present, 32-bit interrupt gate
}

void idt_install(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)(uintptr_t)&idt;
    idt_load(&idt_ptr);
}

void dump_regs(const regs_t* r) {
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x esp=%08lx\n", r->esi, r->edi, r->ebp,
            regs_from_user(r) ? (unsigned long)r->user_esp : (unsigned long)&r->user_esp);
    kprintf("  eip=%08x cs=%04x eflags=%08x err=%08x cr2=%08lx\n",
            r->eip, r->cs, r->eflags, r->err, (unsigned long)read_cr2());
}

uintptr_t arch_task_stack(uintptr_t top, void (*entry)(void)) {
    uintptr_t* sp = (uintptr_t*)top;
    *--sp = (uintptr_t)entry;    // switch_to returns here
    for (int i = 0; i < 4; i++) *--sp = 0;  // ebx, esi, edi, ebp
    return (uintptr_t)sp;
}

void fpu_enable(void) {
    uintptr_t cr0 = read_cr0();
    cr0 &= ~(1u << 2);  // EM: no emulation
    cr0 &= ~(1u << 3);  // TS: never trap on use
    cr0 |= (1u << 1);   // MP
    cr0 |= (1u << 5);   // NE: report x87 errors as exceptions
    write_cr0(cr0);
    __asm__ volatile("fninit");
}

// The x86_64 parts of interrupt and task handling: IDT gates, the
// register dump, the stack a new task starts on, and the FPU.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "console.h"
#include "task.h"

typedef struct {
    uint16_t base_low;
    uint16_t sel;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;

extern void idt_load(idt_ptr_t* p);

void idt_set_gate(int vec, uintptr_t handler, int dpl) {
    idt[vec].base_low = handler & 0xFFFF;
    idt[vec].sel = SEL_KERNEL_CODE;
    idt[vec].ist = 0;
    idt[vec].flags = (uint8_t)(0x8E | dpl << 5);  // present, 64-bit interrupt gate
    idt[vec].base_mid = (handler >> 16) & 0xFFFF;
    idt[vec].base_high = (uint32_t)(handler >> 32);
    idt[vec].zero = 0;
}

void idt_install(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uintptr_t)&idt;
    idt_load(&idt_ptr);
}

void dump_regs(const regs_t* r) {
    kprintf("  rax=%016lx rbx=%016lx rcx=%016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("  rdx=%016lx rsi=%016lx rdi=%016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("  rbp=%016lx rsp=%016lx  r8=%016lx\n", r->rbp, r->rsp, r->r8);
    kprintf("   r9=%016lx r10=%016lx r11=%016lx\n", r->r9, r->r10, r->r11);
    kprintf("  r12=%016lx r13=%016lx r14=%016lx\n", r->r12, r->r13, r->r14);
    kprintf("  r15=%016lx rip=%016lx cs=%04lx\n", r->r15, r->rip, r->cs);
    kprintf("  rflags=%08lx err=%08lx cr2=%016lx\n", r->rflags, r->err, (unsigned long)read_cr2());
}

uintptr_t arch_task_stack(uintptr_t top, void (*entry)(void)) {
    uintptr_t* sp = (uintptr_t*)top;
    *--sp = 0;                   // where a caller's frame would be: entry starts with rsp 8 mod 16
    *--sp = (uintptr_t)entry;    // switch_to returns here
    for (int i = 0; i < 6; i++) *--sp = 0;  // rbp, rbx, r12-r15
    return (uintptr_t)sp;
}

void fpu_enable(void) {
    uintptr_t cr0 = read_cr0();
    cr0 &= ~(uintptr_t)(1u << 2);  // EM: no emulation
    cr0 &= ~(uintptr_t)(1u << 3);  // TS: never trap on use
    cr0 |= 1u << 1;                // MP
    cr0 |= 1u << 5;                // NE: report x87 errors as exceptions
    write_cr0(cr0);
    // OSFXSR: SSE on, and fxsave saves it; OSXMMEXCPT: SIMD errors
    // arrive as exception 19 instead of as invalid opcodes.
    write_cr4(read_cr4() | 1u << 9 | 1u << 10);
    uint32_t mxcsr = 0x1F80;  // the reset value: every SIMD exception masked
    __asm__ volatile("fninit; ldmxcsr %0" :: "m"(mxcsr));
}

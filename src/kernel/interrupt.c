// IDT setup, the PIC, and the single C entry point for every
// interrupt, exception and system call.
#include "kernel.h"
#include "arch.h"
#include "interrupt.h"
#include "console.h"
#include "task.h"
#include "mm.h"
#include "drivers.h"
#include "random.h"
#include "ringtail/abi.h"

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
static irq_handler_t irq_handlers[16];
volatile int need_resched;

extern const uint32_t isr_stub_table[48];
extern const uint32_t isr_syscall_stub;
extern void idt_load(idt_ptr_t* p);

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void idt_set(int vec, uint32_t handler, uint8_t flags) {
    idt[vec].base_low = handler & 0xFFFF;
    idt[vec].base_high = (handler >> 16) & 0xFFFF;
    idt[vec].sel = SEL_KERNEL_CODE;
    idt[vec].zero = 0;
    idt[vec].flags = flags;
}

// Move the PICs' IRQs to vectors 32-47, clear of the CPU exceptions,
// and mask everything. Lines are unmasked one by one as drivers
// register handlers.
static void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE); io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();  // slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();  // 8086 mode
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFB);             // all masked but the cascade
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask(int irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

// In-service register: tells a real IRQ 7/15 from a spurious one.
static uint8_t pic_isr(uint16_t cmd_port) {
    outb(cmd_port, 0x0B);
    return inb(cmd_port);
}

void irq_register(int irq, irq_handler_t handler) {
    uint32_t f = irq_save();
    irq_handlers[irq] = handler;
    pic_unmask(irq);
    irq_restore(f);
}

void idt_init(void) {
    pic_init();
    for (int i = 0; i < 48; i++) idt_set(i, isr_stub_table[i], 0x8E);
    // DPL 3 on the syscall gate so ring 3 may invoke it; it still
    // lands in ring 0 code. Every other gate is DPL 0, so a user
    // "int 14" faults with #GP instead of faking a page fault.
    idt_set(SYSCALL_VECTOR, isr_syscall_stub, 0xEE);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)&idt;
    idt_load(&idt_ptr);
}

static const char* const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow",
    "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor overrun", "invalid TSS",
    "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 error", "alignment check",
    "machine check", "SIMD error", "virtualization", "control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "VMM communication", "security", "reserved",
};

static void dump_regs(const regs_t* r) {
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x esp=%08x\n", r->esi, r->edi, r->ebp,
            regs_from_user(r) ? r->user_esp : (uint32_t)&r->user_esp);
    kprintf("  eip=%08x cs=%04x eflags=%08x err=%08x cr2=%08x\n",
            r->eip, r->cs, r->eflags, r->err, read_cr2());
}

// Walk saved frame pointers. The kernel is built with frame pointers
// so this gives a usable call chain without a symbol table: the
// addresses can be looked up with addr2line or nm.
static void backtrace(uint32_t ebp) {
    kprintf("  backtrace:");
    for (int i = 0; i < 12 && ebp >= (uint32_t)_kernel_start && ebp < RAM_IDENTITY_LIMIT; i++) {
        uint32_t* frame = (uint32_t*)ebp;
        kprintf(" %08x", frame[1]);
        if (frame[0] <= ebp) break;
        ebp = frame[0];
    }
    kprintf("\n");
}

static void kernel_exception(regs_t* r) {
    console_panic_mode();
    gfx_emergency_text();
    kprintf_color(COLOR_ERROR, "\nKERNEL PANIC: %s (exception %u)",
                  exception_names[r->vector], r->vector);
    if (current) kprintf(" in task %d (%s)", current->id, current->name);
    kprintf("\n");
    dump_regs(r);
    backtrace(r->ebp);
    faultlog_add("kernel: %s at eip=%08x cr2=%08x", exception_names[r->vector], r->eip, read_cr2());
    kprintf_color(COLOR_ERROR, "System halted.\n");
    for (;;) { cli(); hlt(); }
}

static void user_exception(regs_t* r) {
    if (r->vector == 14 && user_fault_fixup(read_cr2(), r->err)) return;
    // From here on this is ordinary task context: the frame belongs
    // to the task, so it may block (to print) and then exit.
    sti();
    char what[96];
    if (r->vector == 14) {
        snprintf(what, sizeof(what), "page fault at %08x (%s, %s)", read_cr2(),
                 (r->err & 1) ? "protection" : "not mapped", (r->err & 2) ? "write" : "read");
    } else {
        snprintf(what, sizeof(what), "%s", exception_names[r->vector]);
    }
    faultlog_add("task %d (%s): %s, eip=%08x", current->id, current->name, what, r->eip);
    kprintf_color(COLOR_ERROR, "%s: %s at eip=%08x - task killed\n", current->name, what, r->eip);
    task_exit(EXIT_FAULTED);
}

static void handle_irq(int irq) {
    // A spurious IRQ 7 or 15 is a glitch on the line, not a real
    // request: the in-service bit is clear. It gets no handler, and
    // no EOI from the PIC that raised it (though a spurious 15 still
    // needs one on the master, which did see the cascade).
    if (irq == 7 && !(pic_isr(PIC1_CMD) & 0x80)) return;
    if (irq == 15 && !(pic_isr(PIC2_CMD) & 0x80)) {
        outb(PIC1_CMD, PIC_EOI);
        return;
    }
    random_event((uint32_t)irq);  // when it came is something nobody could have known
    if (irq_handlers[irq]) irq_handlers[irq]();
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void interrupt_dispatch(regs_t* r) {
    if (r->vector < 32) {
        if (regs_from_user(r)) user_exception(r);
        else kernel_exception(r);
    } else if (r->vector < 48) {
        handle_irq((int)r->vector - IRQ_BASE);
    } else if (r->vector == SYSCALL_VECTOR) {
        // System calls run with interrupts on: they may block, and
        // the timer must be able to preempt a long one.
        sti();
        syscall_handle(r);
        cli();
    }
    if (need_resched) {
        need_resched = 0;
        schedule();
    }
    // A kill takes effect here, on the way back to ring 3, where the
    // task holds no kernel locks and has nothing half done.
    if (regs_from_user(r) && current->killed) {
        sti();
        task_exit(EXIT_KILLED);
    }
}

void panic(const char* fmt, ...) {
    cli();
    console_panic_mode();
    gfx_emergency_text();
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf_color(COLOR_ERROR, "\nKERNEL PANIC: %s\n", buf);
    backtrace((uint32_t)__builtin_frame_address(0));
    kprintf_color(COLOR_ERROR, "System halted.\n");
    for (;;) { cli(); hlt(); }
}

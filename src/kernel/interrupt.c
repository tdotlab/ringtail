// IDT setup, the interrupt controller (the 8259 PICs, or the APICs
// when ACPI describes them), and the single C entry point for every
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

static irq_handler_t irq_handlers[16];
// Set once the local and I/O APICs have taken over from the 8259s.
static int apic_on;
static const char* pic_why = "not set up yet";

extern const uintptr_t isr_stub_table[48];
extern const uintptr_t isr_syscall_stub;  // a word holding the stub's address
extern const uintptr_t isr_spurious_stub;
extern const uintptr_t isr_resched_stub;

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

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
    if (apic_on) apic_route(irq);
    else pic_unmask(irq);
    irq_restore(f);
}

void irq_set_handler(int irq, irq_handler_t handler) {
    uint32_t f = irq_save();
    irq_handlers[irq] = handler;
    irq_restore(f);
}

int irq_pending(int irq) {
    if (apic_on) return apic_pending(IRQ_BASE + irq);
    uint16_t cmd = irq < 8 ? PIC1_CMD : PIC2_CMD;
    outb(cmd, 0x0A);  // OCW3: read the request register
    return (inb(cmd) >> (irq & 7)) & 1;
}

void irq_controller_init(void) {
    if (apic_init(&pic_why) < 0) return;
    // Both 8259s masked for good, cascade included. They keep their
    // vectors (32-47), so anything they might still raise lands
    // somewhere harmless.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    apic_on = 1;
    for (int irq = 0; irq < 16; irq++)
        if (irq_handlers[irq]) apic_route(irq);
}

int irq_uses_apic(void) { return apic_on; }

const char* irq_controller(char* buf, uint32_t size) {
    if (apic_on) snprintf(buf, size, "local and I/O APIC");
    else snprintf(buf, size, "8259 PIC (%s)", pic_why);
    return buf;
}

void idt_init(void) {
    pic_init();
    for (int i = 0; i < 48; i++) idt_set_gate(i, isr_stub_table[i], 0);
    // DPL 3 on the syscall gate so ring 3 may invoke it; it still
    // lands in ring 0 code. Every other gate is DPL 0, so a user
    // "int 14" faults with #GP instead of faking a page fault.
    idt_set_gate(SYSCALL_VECTOR, isr_syscall_stub, 3);
    idt_set_gate(0xFF, isr_spurious_stub, 0);
    idt_set_gate(RESCHED_VECTOR, isr_resched_stub, 0);
    idt_install();
}

// ---- One CPU in the kernel at a time ----
//
// The kernel was written for one CPU, and its critical sections only
// shut out interrupts (irq_save), which does nothing about another CPU.
// So a CPU holds this lock whenever it runs kernel code: programs run
// in parallel, in ring 3, and the kernel runs on one CPU at a time.
// The lock belongs to the CPU, not the task: a task that blocks or is
// preempted hands it to the next one on that CPU, and a CPU lets go of
// it only to go back to ring 3 (here, and task_trampoline) or to halt
// in its idle loop. Interrupts stay off while a CPU waits for it, and
// no CPU holding it ever waits for another CPU, so it cannot deadlock.
static volatile int lock_owner = -1;  // a CPU index

int kernel_locked_here(void) { return lock_owner == this_cpu()->index; }

int kernel_trylock(void) {
    int none = -1;
    return __atomic_compare_exchange_n(&lock_owner, &none, this_cpu()->index, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

void kernel_lock(void) {
    while (!kernel_trylock())
        while (__atomic_load_n(&lock_owner, __ATOMIC_RELAXED) != -1) cpu_relax();
}

void kernel_unlock(void) { __atomic_store_n(&lock_owner, -1, __ATOMIC_RELEASE); }

// A panic stops the other CPUs with an NMI, so they cannot go on
// changing what it is about to print; the first CPU to panic is the
// one that reports.
static volatile int panic_cpu = -1;

static void __attribute__((noreturn)) halt_forever(void) {
    for (;;) { cli(); hlt(); }
}

static void stop_others(void) {
    int me = this_cpu()->index, none = -1;
    if (!__atomic_compare_exchange_n(&panic_cpu, &none, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) &&
        none != me)
        halt_forever();  // another CPU is reporting its own
    if (apic_on && smp_cpus() > 1) apic_nmi_others();
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

// Walk saved frame pointers. The kernel is built with frame pointers
// so this gives a usable call chain without a symbol table: the
// addresses can be looked up with addr2line or nm.
static void backtrace(uintptr_t fp) {
    kprintf("  backtrace:");
    for (int i = 0; i < 12 && !(fp & 7) && kernel_mapped(fp) && kernel_mapped(fp + 8); i++) {
        const uintptr_t* frame = (const uintptr_t*)fp;
        kprintf(" %08lx", (unsigned long)frame[1]);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    kprintf("\n");
}

static void kernel_exception(regs_t* r) {
    stop_others();
    console_panic_mode();
    gfx_emergency_text();
    kprintf_color(COLOR_ERROR, "\nKERNEL PANIC: %s (exception %u)",
                  exception_names[r->vector], (unsigned)r->vector);
    if (current) kprintf(" in task %d (%s)", current->id, current->name);
    kprintf("\n");
    dump_regs(r);
    backtrace(regs_fp(r));
    faultlog_add("kernel: %s at " REGS_IP_NAME "=%08lx cr2=%08lx", exception_names[r->vector],
                 (unsigned long)regs_ip(r), (unsigned long)read_cr2());
    kprintf_color(COLOR_ERROR, "System halted.\n");
    halt_forever();
}

static void user_exception(regs_t* r) {
    if (r->vector == 14 && user_fault_fixup(read_cr2(), r->err)) return;
    // From here on this is ordinary task context: the frame belongs
    // to the task, so it may block (to print) and then exit.
    sti();
    char what[96];
    if (r->vector == 14) {
        snprintf(what, sizeof(what), "page fault at %08lx (%s, %s)", (unsigned long)read_cr2(),
                 (r->err & 1) ? "protection" : "not mapped", (r->err & 2) ? "write" : "read");
    } else {
        snprintf(what, sizeof(what), "%s", exception_names[r->vector]);
    }
    faultlog_add("task %d (%s): %s, " REGS_IP_NAME "=%08lx", current->id, current->name, what,
                 (unsigned long)regs_ip(r));
    kprintf_color(COLOR_ERROR, "%s: %s at " REGS_IP_NAME "=%08lx - task killed\n", current->name, what,
                  (unsigned long)regs_ip(r));
    task_exit(EXIT_FAULTED);
}

static void handle_irq(int irq) {
    if (apic_on) {
        // The local APIC's spurious interrupts have a vector of their
        // own (0xFF, a stub that only returns), so whatever is here is
        // real.
        random_event((uint32_t)irq);
        if (irq_handlers[irq]) irq_handlers[irq]();
        apic_eoi();
        return;
    }
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

// NMIs come on a stack of their own (IST) and can interrupt anything,
// a critical section included, so the handler only counts them; the
// next ordinary interrupt logs them, when the count reaches a new
// power of two, so a storm of them cannot push everything else out of
// the fault log. An NMI on a PC is a hardware error report or a
// watchdog, neither of them the running program's fault; or another
// CPU's panic.
static volatile uint32_t nmis_seen;
static uint32_t nmis_logged;

static void handle_nmi(void) {
    if (panic_cpu >= 0 && panic_cpu != this_cpu()->index) halt_forever();
    __atomic_fetch_add(&nmis_seen, 1, __ATOMIC_RELAXED);  // any CPU may take one
}

void interrupt_dispatch(regs_t* r) {
    if (r->vector == 2) {
        handle_nmi();
        return;  // no scheduling, no kill: not on this stack
    }
    int took = 0;  // the lock, on the way in: it is let go on the way out
    if (apic_on && r->vector == IRQ_BASE + IRQ_TIMER) {
        // Each CPU's own timer, which touches only that CPU's block
        // (timer.c), so it needs the lock only to switch tasks; and if
        // another CPU has it, this one keeps running what it runs, and
        // tries again at the next interrupt.
        irq_handlers[IRQ_TIMER]();
        apic_eoi();
        if (!need_resched) return;
        if (!kernel_locked_here()) {
            if (!kernel_trylock()) return;
            took = 1;
        }
    } else if (r->vector == RESCHED_VECTOR) {
        // Another CPU, which has the lock, has a task ready for this
        // one: wait for the lock, then look.
        apic_eoi();
        need_resched = 1;
        if (!kernel_locked_here()) {
            kernel_lock();
            took = 1;
        }
    } else {
        if (!kernel_locked_here()) {
            kernel_lock();
            took = 1;
        }
        uint32_t nmis = nmis_seen;
        if ((nmis ^ nmis_logged) > nmis_logged) {  // a higher top bit than at the last log
            nmis_logged = nmis;
            faultlog_add("NMI received (%u since boot), ignored", (unsigned)nmis);
        }
        // A double fault or machine check, also on their own stacks, ends
        // the system whichever ring it came from.
        if (r->vector == 8 || r->vector == 18) kernel_exception(r);
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
    // took is this frame's, on this task's stack, so it holds after a
    // switch: a frame that came in without the lock goes back where
    // the lock is not held, ring 3 or an idle loop, whichever CPU it
    // leaves from.
    if (took) kernel_unlock();
}

void panic(const char* fmt, ...) {
    cli();
    stop_others();
    console_panic_mode();
    gfx_emergency_text();
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf_color(COLOR_ERROR, "\nKERNEL PANIC: %s\n", buf);
    backtrace((uintptr_t)__builtin_frame_address(0));
    kprintf_color(COLOR_ERROR, "System halted.\n");
    halt_forever();
}

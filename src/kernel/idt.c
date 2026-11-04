// This is how the CPU knows where to jump when a key is pressed

#include <stdint.h>

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idt_ptr;

extern void keyboard_isr(void);
extern void timer_isr(void);
extern void mouse_isr(void);
extern void syscall_isr(void);
extern void page_fault_isr(void);
extern uint32_t exc_vector;
extern void print(const char* s);
extern void print_char(char c);
extern void vga_lock_break(void);
extern void faultlog_add(const char* msg);

// Buffer append helpers, v0.10.3. The handler builds one
// diagnostic string, logs it, then prints it, so the log
// and the screen message are identical by construction.
// Appends are bounded to the buffer size, truncating rather
// than overflowing: the longest exception message is well
// under 64 bytes, but the bound is what makes that safe
// to claim, not just true today.
static void buf_append_str(char* buf, int* idx, const char* s) {
    while (*s && *idx < 63) buf[(*idx)++] = *s++;
}
static void buf_append_dec(char* buf, int* idx, uint32_t n) {
    char tmp[12];
    int i = 0;
    if (n == 0) { if (*idx < 63) buf[(*idx)++] = '0'; return; }
    while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0 && *idx < 63) buf[(*idx)++] = tmp[--i];
}

// Called from exception_common in isr.asm. It never returns to the
// faulting code: the common stub halts after this returns, so the
// error code the CPU may have pushed for some vectors is irrelevant.
extern int task_current_is_user(void);
extern void task_exit(void);

void exception_handler(void) {
    // v0.5.1: break the print lock before printing. If the fault
    // happened while a task held it, printing here would otherwise
    // spin forever on a lock held by a task that will never run
    // again, and the diagnostic would never appear.
    // v0.7.1: same recovery rule as page faults. A user program
    // that faults (a privileged instruction, a divide by zero,
    // any generic exception) is killed via task_exit, which
    // switches away and never returns, so the caller of this
    // handler (exception_common, with its popa and hang) never
    // runs for a user fault. A kernel task's fault still returns
    // to that hang, as before: a kernel fault is an OS bug, and
    // halting with a diagnostic is the honest ending for it.
    // v0.10.3: build one diagnostic string, log it, then print
    // it, so the fault log and the screen agree exactly, and
    // a graphics-mode fault is still readable from `faults`
    // after the text restore wipes the screen version. The
    // log call comes before task_exit, which never returns.
    vga_lock_break();
    int is_user = task_current_is_user();
    char buf[64];
    int idx = 0;
    buf_append_str(buf, &idx, "EXCEPTION ");
    buf_append_dec(buf, &idx, exc_vector);
    if (is_user) buf_append_str(buf, &idx, " - task killed.");
    else buf_append_str(buf, &idx, " - system halted.");
    buf[idx] = 0;
    faultlog_add(buf);
    print("\n");
    print(buf);
    print("\n");
    if (is_user) {
        task_exit(); // never returns
        return; // backstop, unreachable
    }
}

// port I/O - how we talk to PIC and keyboard hardware
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

// PIC remap - moves IRQs from 0-15 to 32-47 so they don't clash with CPU faults
static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20); // master offset 32
    outb(0xA1, 0x28); // slave offset 40
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // Mask everything except IRQ0 (timer), IRQ1 (keyboard) and IRQ2
    // (cascade, needed for the slave PIC to signal at all). The slave
    // is masked except IRQ4 (the PS/2 mouse, IRQ 12): 0xEF is
    // 11101111, bit 4 clear, everything else masked. Leaving all
    // IRQs unmasked with only two IDT gates present meant any stray
    // IRQ triple-faulted the CPU.
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}

extern void exception_0(void);
extern void exception_1(void);
extern void exception_2(void);
extern void exception_3(void);
extern void exception_4(void);
extern void exception_5(void);
extern void exception_6(void);
extern void exception_7(void);
extern void exception_8(void);
extern void exception_9(void);
extern void exception_10(void);
extern void exception_11(void);
extern void exception_12(void);
extern void exception_13(void);
extern void exception_14(void);
extern void exception_15(void);
extern void exception_16(void);
extern void exception_17(void);
extern void exception_18(void);
extern void exception_19(void);
extern void exception_20(void);
extern void exception_21(void);
extern void exception_22(void);
extern void exception_23(void);
extern void exception_24(void);
extern void exception_25(void);
extern void exception_26(void);
extern void exception_27(void);
extern void exception_28(void);
extern void exception_29(void);
extern void exception_30(void);
extern void exception_31(void);

extern void idt_load(uint32_t);

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)&idt;

    // clear IDT
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    pic_remap();

    // IRQ1 (keyboard) is now interrupt 33
    idt_set_gate(33, (uint32_t)keyboard_isr, 0x08, 0x8E);

    // IRQ12 (PS/2 mouse) is now interrupt 44: slave offset 40 plus
    // slave IRQ 4. Without this gate, the first mouse movement
    // after sti would hit a non-present gate and triple-fault.
    idt_set_gate(44, (uint32_t)mouse_isr, 0x08, 0x8E);

    // IRQ0 (PIT timer) is now interrupt 32
    idt_set_gate(32, (uint32_t)timer_isr, 0x08, 0x8E);

    // v0.6.0: syscall gate, interrupt 0x80. Flags 0xEE, not 0x8E:
    // the DPL bits are 3, not 0, and that is the whole point. A
    // ring 3 program may only invoke an interrupt gate whose DPL
    // is at least its own privilege; a DPL 0 gate would fault
    // with #GP the moment user code tried int 0x80, and the
    // syscall doorway would be locked to the very programs it
    // exists for. The gate still points at kernel code (0x08),
    // so the trap lands in ring 0; the DPL only controls who
    // may knock, not where the knock goes.
    idt_set_gate(128, (uint32_t)syscall_isr, 0x08, 0xEE);

    // CPU exceptions 0-31: without these, any fault (divide by zero,
    // general protection, page fault, ...) hit a non-present gate and
    // triple-faulted with no diagnostic at all.
    {
        void (*exc_stubs[32])(void) = {exception_0, exception_1, exception_2, exception_3, exception_4, exception_5, exception_6, exception_7, exception_8, exception_9, exception_10, exception_11, exception_12, exception_13, exception_14, exception_15, exception_16, exception_17, exception_18, exception_19, exception_20, exception_21, exception_22, exception_23, exception_24, exception_25, exception_26, exception_27, exception_28, exception_29, exception_30, exception_31};
        for (int i = 0; i < 32; i++) {
            idt_set_gate(i, (uint32_t)exc_stubs[i], 0x08, 0x8E);
        }
    }

    // v0.7.0: page faults get their own handler (vector 14), not
    // the generic exception stub set in the loop above, because
    // the fault address and error code are the diagnostics that
    // tell a paging bug from any other fault. This must come
    // AFTER the loop, or the loop overwrites it.
    idt_set_gate(14, (uint32_t)page_fault_isr, 0x08, 0x8E);

    idt_load((uint32_t)&idt_ptr);
}

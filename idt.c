// RingTail - Piece 5: IDT (Interrupt Descriptor Table)
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
extern uint32_t exc_vector;
extern void print(const char* s);
extern void print_char(char c);

static void print_dec(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print_char('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) { print_char(buf[--i]); }
}

// Called from exception_common in isr.asm. It never returns to the
// faulting code: the common stub halts after this returns, so the
// error code the CPU may have pushed for some vectors is irrelevant.
void exception_handler(void) {
    print("\nEXCEPTION ");
    print_dec(exc_vector);
    print(" - system halted.\n");
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
    // is fully masked. Leaving all IRQs unmasked with only two IDT
    // gates present meant any stray IRQ triple-faulted the CPU.
    outb(0x21, 0xF8);
    outb(0xA1, 0xFF);
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

    // IRQ0 (PIT timer) is now interrupt 32
    idt_set_gate(32, (uint32_t)timer_isr, 0x08, 0x8E);

    // CPU exceptions 0-31: without these, any fault (divide by zero,
    // general protection, page fault, ...) hit a non-present gate and
    // triple-faulted with no diagnostic at all.
    {
        void (*exc_stubs[32])(void) = {exception_0, exception_1, exception_2, exception_3, exception_4, exception_5, exception_6, exception_7, exception_8, exception_9, exception_10, exception_11, exception_12, exception_13, exception_14, exception_15, exception_16, exception_17, exception_18, exception_19, exception_20, exception_21, exception_22, exception_23, exception_24, exception_25, exception_26, exception_27, exception_28, exception_29, exception_30, exception_31};
        for (int i = 0; i < 32; i++) {
            idt_set_gate(i, (uint32_t)exc_stubs[i], 0x08, 0x8E);
        }
    }

    idt_load((uint32_t)&idt_ptr);
}

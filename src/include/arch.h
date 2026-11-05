// x86 primitives: port I/O, interrupt flag, control registers.
#ifndef RINGTAIL_ARCH_H
#define RINGTAIL_ARCH_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// A write to an unused port takes about 1 microsecond: the classic
// delay between PIC programming steps.
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

#define EFLAGS_IF 0x200

static inline uint32_t read_eflags(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0" : "=r"(f) :: "memory");
    return f;
}

// Nestable critical section: disables interrupts and returns the
// previous state, so an inner restore never re-enables early.
static inline uint32_t irq_save(void) {
    uint32_t f = read_eflags();
    cli();
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & EFLAGS_IF) sti();
}
static inline int irqs_enabled(void) { return (read_eflags() & EFLAGS_IF) != 0; }

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr4(void) { uint32_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint32_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline void write_cr3(uint32_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void write_cr4(uint32_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

static inline void invlpg(uint32_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

#endif

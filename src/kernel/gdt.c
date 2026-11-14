// Global descriptor table and task state segment.
//
// Flat segments for kernel and user code and data, plus the TSS.
// The TSS is used for one thing only: esp0, the kernel stack the
// CPU switches to when a trap arrives from ring 3. The scheduler
// points it at the running task's kernel stack on every switch.
#include "kernel.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed)) tss_t;

static gdt_entry_t gdt[6];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

extern void gdt_load(gdt_ptr_t* p);

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].base_low = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].access = access;
    gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].base_high = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);  // 0x08 kernel code
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);  // 0x10 kernel data
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);  // 0x18 user code (DPL 3)
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);  // 0x20 user data (DPL 3)
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = SEL_KERNEL_DATA;
    // An I/O bitmap offset past the segment limit means there is no
    // bitmap, so with IOPL 0 every port access from ring 3 faults.
    tss.iomap_base = sizeof(tss);
    set_entry(5, (uint32_t)(uintptr_t)&tss, sizeof(tss) - 1, 0x89, 0x00);  // 0x28 TSS
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)(uintptr_t)&gdt;
    gdt_load(&gdt_ptr);
}

void tss_set_kstack(uintptr_t top) {
    tss.esp0 = top;
}

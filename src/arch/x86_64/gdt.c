// Global descriptor table and task state segment, x86_64.
//
// Flat segments for kernel and user code and data, plus the TSS. In
// long mode segments do little: the code segments say 64-bit and which
// ring, and the TSS holds rsp0, the kernel stack the CPU switches to
// when a trap arrives from ring 3. The scheduler points it at the
// running task's kernel stack on every switch.
#include "kernel.h"

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// Five 8-byte descriptors, then the TSS's, which takes two slots.
static uint64_t gdt[7];
static gdt_ptr_t gdt_ptr;
static tss_t tss;

extern void gdt_load(gdt_ptr_t* p);

// Base 0 and the largest limit; flags is the top nibble: G, D/B, L, AVL.
static uint64_t segment(uint8_t access, uint8_t flags) {
    return 0xFFFFull | (uint64_t)access << 40 | 0xFull << 48 | (uint64_t)flags << 52;
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = segment(0x9A, 0xA);  // 0x08 kernel code, 64-bit
    gdt[2] = segment(0x92, 0xC);  // 0x10 kernel data
    gdt[3] = segment(0xFA, 0xA);  // 0x18 user code, 64-bit (DPL 3)
    gdt[4] = segment(0xF2, 0xC);  // 0x20 user data (DPL 3)
    memset(&tss, 0, sizeof(tss));
    // An I/O bitmap offset past the segment limit means there is no
    // bitmap, so with IOPL 0 every port access from ring 3 faults.
    tss.iomap_base = sizeof(tss);
    uint64_t base = (uintptr_t)&tss, limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | 0x89ull << 40 | (limit >> 16 & 0xF) << 48 |
             (base >> 24 & 0xFF) << 56;   // 0x28 TSS, available
    gdt[6] = base >> 32;
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uintptr_t)&gdt;
    gdt_load(&gdt_ptr);
}

void tss_set_kstack(uintptr_t top) {
    tss.rsp0 = top;
}

// Global descriptor table, task state segment and the SYSCALL MSRs,
// x86_64.
//
// Flat segments for kernel and user code and data, plus the TSS. In
// long mode segments do little: the code segments say 64-bit and which
// ring, and the TSS holds rsp0, the kernel stack the CPU switches to
// when a trap arrives from ring 3, and the IST stacks below. The
// scheduler points rsp0 at the running task's kernel stack on every
// switch.
//
// The SYSCALL instruction is set up here too, because its MSRs are
// tied to the GDT's layout: SYSRET loads the user selectors from
// STAR + 8 and STAR + 16, so user data has to come before user code.
#include "kernel.h"
#include "arch.h"
#include "arch/percpu.h"

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

// Every CPU has its own GDT, since loading the task register marks the
// TSS's descriptor busy, and its own TSS and interrupt stacks, since
// two CPUs can take a trap at once; and its block (arch/percpu.h),
// first, so a cpu_t* is also a pointer to the rest. Index 0 is the
// boot CPU.
typedef struct {
    cpu_t cpu;
    // Five 8-byte descriptors, then the TSS's, which takes two slots.
    uint64_t gdt[7];
    gdt_ptr_t gdt_ptr;
    tss_t tss;
} cpu_tables_t;
static cpu_tables_t cpus[MAX_CPUS];

// Stacks for the traps that may arrive when the stack cannot be
// trusted (IST_* in kernel.h): an NMI or a machine check in the few
// instructions where the SYSCALL entry or exit runs in ring 0 on the
// user's rsp, and a double fault, which is often a kernel stack gone
// bad. The CPU switches to these whatever the ring. The boot CPU's are
// here; the others' come from the heap (smp.c).
static uint8_t bsp_ist_stacks[IST_STACKS_SIZE] __attribute__((aligned(16)));

extern void gdt_load(gdt_ptr_t* p);
extern void syscall_entry(void);

#define MSR_EFER           0xC0000080
#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_FMASK          0xC0000084
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SCE           1u

// Base 0 and the largest limit; flags is the top nibble: G, D/B, L, AVL.
static uint64_t segment(uint8_t access, uint8_t flags) {
    return 0xFFFFull | (uint64_t)access << 40 | 0xFull << 48 | (uint64_t)flags << 52;
}

static void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    // SYSCALL takes CS from STAR[47:32] and SS from 8 past it: 0x08 and
    // 0x10. SYSRET takes SS from STAR[63:48] + 8 and CS from + 16, both
    // with RPL 3: 0x1B and 0x23.
    wrmsr(MSR_STAR, (uint64_t)(SEL_USER_DATA - 8) << 48 | (uint64_t)SEL_KERNEL_CODE << 32);
    wrmsr(MSR_LSTAR, (uintptr_t)syscall_entry);
    // Flags cleared on entry: TF (no single step into the kernel), IF
    // (the entry runs on the user's stack until it has switched), DF
    // (the C code expects it clear), IOPL, NT and AC. SYSRET puts the
    // user's back from r11.
    wrmsr(MSR_FMASK, 0x100 | 0x200 | 0x400 | 0x3000 | 0x4000 | 0x40000);
}

cpu_t* cpu_get(int index) { return &cpus[index].cpu; }

void gdt_init_cpu(int index, void* ist_stacks) {
    cpu_tables_t* c = &cpus[index];
    uint64_t* gdt = c->gdt;
    gdt[0] = 0;
    gdt[1] = segment(0x9A, 0xA);  // 0x08 kernel code, 64-bit
    gdt[2] = segment(0x92, 0xC);  // 0x10 kernel data
    gdt[3] = segment(0xF2, 0xC);  // 0x18 user data (DPL 3), selector 0x1B
    gdt[4] = segment(0xFA, 0xA);  // 0x20 user code, 64-bit (DPL 3), selector 0x23
    tss_t* tss = &c->tss;
    memset(tss, 0, sizeof(*tss));
    // An I/O bitmap offset past the segment limit means there is no
    // bitmap, so with IOPL 0 every port access from ring 3 faults.
    tss->iomap_base = sizeof(*tss);
    uint8_t* ist = ist_stacks;
    for (int i = 0; i < 3; i++) tss->ist[i] = (uintptr_t)ist + (uintptr_t)(i + 1) * IST_STACK_SIZE;  // IST1-3
    uint64_t base = (uintptr_t)tss, limit = sizeof(*tss) - 1;
    gdt[5] = (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | 0x89ull << 40 | (limit >> 16 & 0xF) << 48 |
             (base >> 24 & 0xFF) << 56;   // 0x28 TSS, available
    gdt[6] = base >> 32;
    c->gdt_ptr.limit = sizeof(c->gdt) - 1;
    c->gdt_ptr.base = (uintptr_t)gdt;
    gdt_load(&c->gdt_ptr);
    syscall_init();
    // gs's base is this CPU's block from here on, in the kernel; the
    // program's is 0, kept in KERNEL_GS_BASE while the kernel runs and
    // swapped in on the way out. Programs cannot change theirs but by
    // loading a selector, whose base is 0 (no FSGSBASE, no wrmsr in
    // ring 3). This comes after gdt_load, which loads gs, and that can
    // clear the base.
    c->cpu.self = &c->cpu;
    c->cpu.index = index;
    wrmsr(MSR_GS_BASE, (uintptr_t)&c->cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void gdt_init(void) { gdt_init_cpu(0, bsp_ist_stacks); }

// For the CPU it runs on, with interrupts off: the scheduler, as it
// switches.
void tss_set_kstack(uintptr_t top) {
    cpu_tables_t* c = (cpu_tables_t*)this_cpu();
    c->tss.rsp0 = top;
    c->cpu.kstack = top;
}

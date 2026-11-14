// Page tables, i386: a page directory and page tables of 1024 32-bit
// entries each, with 4 MB pages for most of the kernel's RAM.
//
// The layout is the one in mm.h. Kernel entries are the same in every
// page directory (copied from the kernel directory, and never changed
// after boot), so the kernel keeps running across address-space
// switches. The device window's page table is shared, so mappings
// added to it reach every address space.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "ringtail/abi.h"

#define PDE_INDEX(va) ((va) >> 22)
#define PTE_INDEX(va) (((va) >> 12) & 0x3FF)
#define FRAME(e) ((e) & ~(pte_t)0xFFF)
#define KERNEL_PDES (int)(USER_BASE >> 22)  // 256
#define USER_PDE_END (int)(USER_TOP >> 22)  // 768

static pte_t kpd[1024] __attribute__((aligned(4096)));
static pte_t kpt0[1024] __attribute__((aligned(4096)));
static pte_t kpt_mmio[1024] __attribute__((aligned(4096)));
static uintptr_t mmio_next = MMIO_WINDOW;
static pte_t global_flag;
#define MMIO_PDE PDE_INDEX(MMIO_WINDOW)
#define PTE_PWT 0x008
#define PTE_PCD 0x010

uintptr_t kernel_pd(void) { return virt_to_phys(kpd); }

void paging_init(uintptr_t ram_top) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 3))) panic("CPU has no 4 MB pages (PSE); RingTail needs a Pentium or later");
    int pge = (d & (1u << 13)) != 0;
    pte_t global = pge ? PTE_GLOBAL : 0;
    global_flag = global;
    cpuid(0, &a, &b, &c, &d);
    int smep = 0;
    if (a >= 7) {
        cpuid(7, &a, &b, &c, &d);
        smep = (b & (1u << 7)) != 0;
    }

    uintptr_t ro_start = virt_to_phys(_ro_start), ro_end = virt_to_phys(_ro_end);
    if (ro_end > 0x400000) panic("kernel image does not fit in the first 4 MB");
    if (ram_top > RAM_IDENTITY_LIMIT) ram_top = RAM_IDENTITY_LIMIT;

    // First 4 MB with 4 KB pages: page 0 unmapped, kernel text and
    // rodata read-only.
    for (uintptr_t i = 1; i < 1024; i++) {
        uintptr_t addr = i * PAGE_SIZE;
        pte_t flags = PTE_PRESENT | global;
        if (addr < ro_start || addr >= ro_end) flags |= PTE_WRITE;
        kpt0[i] = addr | flags;
    }
    kpd[0] = virt_to_phys(kpt0) | PTE_PRESENT | PTE_WRITE;
    // The rest of RAM with 4 MB pages.
    uintptr_t big = (ram_top + 0x3FFFFF) >> 22;
    for (uintptr_t i = 1; i < big && i < (uintptr_t)KERNEL_PDES; i++)
        kpd[i] = (i << 22) | PTE_PRESENT | PTE_WRITE | PTE_PS | global;
    kpd[MMIO_PDE] = virt_to_phys(kpt_mmio) | PTE_PRESENT | PTE_WRITE;

    uintptr_t cr4 = read_cr4() | (1u << 4);  // PSE
    if (pge) cr4 |= 1u << 7;
    if (smep) cr4 |= 1u << 20;              // kernel may never execute user pages
    write_cr4(cr4);
    write_cr3(virt_to_phys(kpd));
    // PG on, and WP: the kernel honours read-only pages too, so a
    // stray kernel write to its own code faults instead of patching it.
    write_cr0(read_cr0() | 0x80000000u | (1u << 16));
}

void* mmio_map(uintptr_t phys, uint32_t size) {
    uintptr_t off = phys & (PAGE_SIZE - 1);
    uintptr_t pages = (off + size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!size || pages > (MMIO_WINDOW + MMIO_WINDOW_SIZE - mmio_next) / PAGE_SIZE) return 0;
    uintptr_t va = mmio_next;
    for (uintptr_t i = 0; i < pages; i++) {
        kpt_mmio[PTE_INDEX(va) + i] = (ALIGN_DOWN(phys, PAGE_SIZE) + i * PAGE_SIZE)
                                      | PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT | global_flag;
        invlpg(va + i * PAGE_SIZE);
    }
    mmio_next += pages * PAGE_SIZE;
    return (void*)(va + off);
}

uintptr_t as_create(void) {
    uintptr_t pdp = frame_alloc();
    if (!pdp) return 0;
    pte_t* pd = phys_to_virt(pdp);
    for (int i = 0; i < KERNEL_PDES; i++) pd[i] = kpd[i];
    pd[MMIO_PDE] = kpd[MMIO_PDE];
    return pdp;
}

void as_destroy(uintptr_t pdp) {
    pte_t* pd = phys_to_virt(pdp);
    for (int i = KERNEL_PDES; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        pte_t* pt = phys_to_virt(FRAME(pd[i]));
        for (int j = 0; j < 1024; j++)
            if (pt[j] & PTE_PRESENT) frame_free(FRAME(pt[j]));
        frame_free(FRAME(pd[i]));
    }
    frame_free(pdp);
}

static pte_t* pte_for(uintptr_t pdp, uintptr_t va, int create) {
    pte_t* pd = phys_to_virt(pdp);
    pte_t* pde = &pd[PDE_INDEX(va)];
    if (!(*pde & PTE_PRESENT)) {
        if (!create) return 0;
        uintptr_t pt = frame_alloc();
        if (!pt) return 0;
        *pde = pt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return &((pte_t*)phys_to_virt(FRAME(*pde)))[PTE_INDEX(va)];
}

uint32_t as_pages(uintptr_t pdp) {
    pte_t* pd = phys_to_virt(pdp);
    uint32_t n = 0;
    for (int i = KERNEL_PDES; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        pte_t* pt = phys_to_virt(FRAME(pd[i]));
        for (int j = 0; j < 1024; j++) n += (pt[j] & PTE_PRESENT) != 0;
    }
    return n;
}

uintptr_t as_map(uintptr_t pd, uintptr_t va, int writable) {
    if (va < USER_BASE || va >= USER_TOP || (va & (PAGE_SIZE - 1))) return 0;
    pte_t* pte = pte_for(pd, va, 1);
    if (!pte) return 0;
    if (*pte & PTE_PRESENT) {
        if (writable && !(*pte & PTE_WRITE)) {
            *pte |= PTE_WRITE;
            if (read_cr3() == pd) invlpg(va);
        }
        return FRAME(*pte);
    }
    uintptr_t f = frame_alloc();
    if (!f) return 0;
    *pte = f | PTE_PRESENT | PTE_USER | (writable ? PTE_WRITE : 0);
    return f;
}

uintptr_t as_lookup(uintptr_t pd, uintptr_t va, int* writable) {
    if (va < USER_BASE || va >= USER_TOP) return 0;
    pte_t* pte = pte_for(pd, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    if (writable) *writable = (*pte & PTE_WRITE) != 0;
    return FRAME(*pte);
}

// Page tables, x86_64: four levels (PML4, PDPT, page directory, page
// table) of 512 64-bit entries each.
//
// The layout (mm.h) lies inside the first 4 GB, so every address space
// uses one PML4 entry, pointing at its own PDPT:
//   PDPT[0]  0 - 1 GB   kernel: one page directory, shared by all
//   PDPT[1]  1 - 2 GB   user, private
//   PDPT[2]  2 - 3 GB   user, private
//   PDPT[3]  3 - 4 GB   device window: one page directory, shared
// The shared directories are never replaced after boot, so the kernel
// keeps running across address-space switches, and device mappings
// added later reach every address space. An address space is named by
// the physical address of its PML4, which is what goes in CR3.
//
// Writable user pages are not executable (when the CPU has NX), nor is
// anything of the kernel's but its text.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "ringtail/abi.h"

#define IDX(va, shift) ((int)(((va) >> (shift)) & 0x1FF))
#define PDPT_INDEX(va) IDX(va, 30)
#define PD_INDEX(va) IDX(va, 21)
#define PT_INDEX(va) IDX(va, 12)
#define FRAME(e) ((e) & 0x000FFFFFFFFFF000ull)
#define PTE_PWT 0x008
#define PTE_PCD 0x010
#define PTE_NX (1ull << 63)
#define USER_PDPT_FIRST (int)(USER_BASE >> 30)  // 1
#define USER_PDPT_END (int)(USER_TOP >> 30)     // 3
#define LOW_TABLES 2                             // the first 4 MB in 4 KB pages

#define TABLE static pte_t __attribute__((aligned(4096)))
TABLE kpml4[512];
TABLE kpdpt[512];
TABLE kpd[512];
TABLE kpt_low[LOW_TABLES][512];
TABLE kpd_mmio[512];
TABLE kpt_mmio[MMIO_WINDOW_SIZE >> 21][512];
static uintptr_t mmio_next = MMIO_WINDOW;
static pte_t global_flag, nx_flag;

extern char _text_end[];

uintptr_t kernel_pd(void) { return virt_to_phys(kpml4); }

void paging_init(uintptr_t ram_top) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    int pge = (d & (1u << 13)) != 0;
    global_flag = pge ? PTE_GLOBAL : 0;
    cpuid(0, &a, &b, &c, &d);
    int smep = 0;
    if (a >= 7) {
        cpuid(7, &a, &b, &c, &d);
        smep = (b & (1u << 7)) != 0;
    }
    cpuid(0x80000001, &a, &b, &c, &d);
    int nx = (d & (1u << 20)) != 0;
    nx_flag = nx ? PTE_NX : 0;

    uintptr_t text_end = virt_to_phys(_text_end);
    uintptr_t ro_start = virt_to_phys(_ro_start), ro_end = virt_to_phys(_ro_end);
    if (ro_end > LOW_TABLES * 0x200000u) panic("kernel image does not fit in the first 4 MB");
    if (ram_top > RAM_IDENTITY_LIMIT) ram_top = RAM_IDENTITY_LIMIT;

    // First 4 MB with 4 KB pages: page 0 unmapped, kernel text and
    // rodata read-only, only text executable.
    for (uintptr_t i = 1; i < LOW_TABLES * 512; i++) {
        uintptr_t addr = i * PAGE_SIZE;
        pte_t flags = PTE_PRESENT | global_flag;
        if (addr < ro_start || addr >= ro_end) flags |= PTE_WRITE;
        if (addr < ro_start || addr >= text_end) flags |= nx_flag;
        kpt_low[i / 512][i % 512] = addr | flags;
    }
    for (int i = 0; i < LOW_TABLES; i++) kpd[i] = virt_to_phys(kpt_low[i]) | PTE_PRESENT | PTE_WRITE;
    // The rest of RAM with 2 MB pages.
    uintptr_t big = (ram_top + 0x1FFFFF) >> 21;
    for (uintptr_t i = LOW_TABLES; i < big && i < 512; i++)
        kpd[i] = (i << 21) | PTE_PRESENT | PTE_WRITE | PTE_PS | global_flag | nx_flag;
    for (int i = 0; i < (int)(MMIO_WINDOW_SIZE >> 21); i++)
        kpd_mmio[PD_INDEX(MMIO_WINDOW) + i] = virt_to_phys(kpt_mmio[i]) | PTE_PRESENT | PTE_WRITE;
    kpdpt[0] = virt_to_phys(kpd) | PTE_PRESENT | PTE_WRITE;
    kpdpt[PDPT_INDEX(MMIO_WINDOW)] = virt_to_phys(kpd_mmio) | PTE_PRESENT | PTE_WRITE;
    // User access needs the user bit at every level; the levels below
    // decide, so it is set here for all.
    kpml4[0] = virt_to_phys(kpdpt) | PTE_PRESENT | PTE_WRITE | PTE_USER;

    if (nx) wrmsr(0xC0000080, rdmsr(0xC0000080) | 1u << 11);  // EFER.NXE
    uintptr_t cr4 = read_cr4();
    if (pge) cr4 |= 1u << 7;
    if (smep) cr4 |= 1u << 20;  // kernel may never execute user pages
    write_cr4(cr4);
    write_cr3(virt_to_phys(kpml4));
    // WP: the kernel honours read-only pages too, so a stray kernel
    // write to its own code faults instead of patching it.
    write_cr0(read_cr0() | 1u << 16);
}

void* mmio_map(uintptr_t phys, uint32_t size) {
    uintptr_t off = phys & (PAGE_SIZE - 1);
    uintptr_t pages = (off + size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!size || pages > (MMIO_WINDOW + MMIO_WINDOW_SIZE - mmio_next) / PAGE_SIZE) return 0;
    uintptr_t va = mmio_next;
    for (uintptr_t i = 0; i < pages; i++) {
        uintptr_t v = va + i * PAGE_SIZE;
        kpt_mmio[PD_INDEX(v) - PD_INDEX(MMIO_WINDOW)][PT_INDEX(v)] =
            (ALIGN_DOWN(phys, PAGE_SIZE) + i * PAGE_SIZE) | PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT |
            global_flag | nx_flag;
        invlpg(v);
    }
    mmio_next += pages * PAGE_SIZE;
    return (void*)(va + off);
}

uintptr_t as_create(void) {
    uintptr_t pml4 = frame_alloc();
    if (!pml4) return 0;
    uintptr_t pdpt = frame_alloc();
    if (!pdpt) { frame_free(pml4); return 0; }
    pte_t* l3 = phys_to_virt(pdpt);
    l3[0] = kpdpt[0];
    l3[PDPT_INDEX(MMIO_WINDOW)] = kpdpt[PDPT_INDEX(MMIO_WINDOW)];
    ((pte_t*)phys_to_virt(pml4))[0] = pdpt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return pml4;
}

static pte_t* user_pdpt(uintptr_t pml4) {
    return phys_to_virt(FRAME(((pte_t*)phys_to_virt(pml4))[0]));
}

void as_destroy(uintptr_t pml4) {
    pte_t* l3 = user_pdpt(pml4);
    for (int i = USER_PDPT_FIRST; i < USER_PDPT_END; i++) {
        if (!(l3[i] & PTE_PRESENT)) continue;
        pte_t* pd = phys_to_virt(FRAME(l3[i]));
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT)) continue;
            pte_t* pt = phys_to_virt(FRAME(pd[j]));
            for (int k = 0; k < 512; k++)
                if (pt[k] & PTE_PRESENT) frame_free(FRAME(pt[k]));
            frame_free(FRAME(pd[j]));
        }
        frame_free(FRAME(l3[i]));
    }
    frame_free(virt_to_phys(l3));
    frame_free(pml4);
}

// The table an entry points at, made (empty) when create is set.
static pte_t* next_table(pte_t* e, int create) {
    if (!(*e & PTE_PRESENT)) {
        if (!create) return 0;
        uintptr_t t = frame_alloc();
        if (!t) return 0;
        *e = t | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return phys_to_virt(FRAME(*e));
}

// Callers check that va is in the user range.
static pte_t* pte_for(uintptr_t pml4, uintptr_t va, int create) {
    pte_t* pd = next_table(&user_pdpt(pml4)[PDPT_INDEX(va)], create);
    if (!pd) return 0;
    pte_t* pt = next_table(&pd[PD_INDEX(va)], create);
    if (!pt) return 0;
    return &pt[PT_INDEX(va)];
}

uint32_t as_pages(uintptr_t pml4) {
    pte_t* l3 = user_pdpt(pml4);
    uint32_t n = 0;
    for (int i = USER_PDPT_FIRST; i < USER_PDPT_END; i++) {
        if (!(l3[i] & PTE_PRESENT)) continue;
        pte_t* pd = phys_to_virt(FRAME(l3[i]));
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT)) continue;
            pte_t* pt = phys_to_virt(FRAME(pd[j]));
            for (int k = 0; k < 512; k++) n += (pt[k] & PTE_PRESENT) != 0;
        }
    }
    return n;
}

uintptr_t as_map(uintptr_t pml4, uintptr_t va, int writable) {
    if (va < USER_BASE || va >= USER_TOP || (va & (PAGE_SIZE - 1))) return 0;
    pte_t* pte = pte_for(pml4, va, 1);
    if (!pte) return 0;
    if (*pte & PTE_PRESENT) {
        // Widened, but left executable: it may hold code too.
        if (writable && !(*pte & PTE_WRITE)) {
            *pte |= PTE_WRITE;
            if (read_cr3() == pml4) invlpg(va);
        }
        return FRAME(*pte);
    }
    uintptr_t f = frame_alloc();
    if (!f) return 0;
    *pte = f | PTE_PRESENT | PTE_USER | (writable ? PTE_WRITE | nx_flag : 0);
    return f;
}

uintptr_t as_lookup(uintptr_t pml4, uintptr_t va, int* writable) {
    if (va < USER_BASE || va >= USER_TOP) return 0;
    pte_t* pte = pte_for(pml4, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    if (writable) *writable = (*pte & PTE_WRITE) != 0;
    return FRAME(*pte);
}

// Paging: the kernel's address space, per-task user address spaces,
// stack growth, and checked access to user memory.
//
// Layout of every address space:
//   0 - 4 KB      not mapped, so null pointers fault
//   4 KB - 1 GB   kernel: identity-mapped RAM, supervisor only,
//                 kernel code and read-only data write-protected
//   1 GB - 3 GB   user: program image, then a 1 MB stack region
//                 just below 3 GB that grows on demand
//   3 GB - 4 GB   not mapped
// Kernel entries are the same in every page directory (copied from
// the kernel directory, and never changed after boot), so the kernel
// keeps running across address-space switches.
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "task.h"
#include "ringtail/abi.h"

#define PDE_INDEX(va) ((va) >> 22)
#define PTE_INDEX(va) (((va) >> 12) & 0x3FF)
#define FRAME(e) ((e) & ~0xFFFu)
#define KERNEL_PDES (int)(USER_BASE >> 22)  // 256
#define USER_PDE_END (int)(USER_TOP >> 22)  // 768

static uint32_t kpd[1024] __attribute__((aligned(4096)));
static uint32_t kpt0[1024] __attribute__((aligned(4096)));

uint32_t kernel_pd(void) { return (uint32_t)kpd; }

void paging_init(uint32_t ram_top) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 3))) panic("CPU has no 4 MB pages (PSE); RingTail needs a Pentium or later");
    int pge = (d & (1u << 13)) != 0;
    uint32_t global = pge ? PTE_GLOBAL : 0;
    cpuid(0, &a, &b, &c, &d);
    int smep = 0;
    if (a >= 7) {
        cpuid(7, &a, &b, &c, &d);
        smep = (b & (1u << 7)) != 0;
    }

    if ((uint32_t)_ro_end > 0x400000) panic("kernel image does not fit in the first 4 MB");
    if (ram_top > RAM_IDENTITY_LIMIT) ram_top = RAM_IDENTITY_LIMIT;

    // First 4 MB with 4 KB pages: page 0 unmapped, kernel text and
    // rodata read-only.
    for (uint32_t i = 1; i < 1024; i++) {
        uint32_t addr = i * PAGE_SIZE;
        uint32_t flags = PTE_PRESENT | global;
        if (addr < (uint32_t)_ro_start || addr >= (uint32_t)_ro_end) flags |= PTE_WRITE;
        kpt0[i] = addr | flags;
    }
    kpd[0] = (uint32_t)kpt0 | PTE_PRESENT | PTE_WRITE;
    // The rest of RAM with 4 MB pages.
    uint32_t big = (ram_top + 0x3FFFFF) >> 22;
    for (uint32_t i = 1; i < big && i < (uint32_t)KERNEL_PDES; i++)
        kpd[i] = (i << 22) | PTE_PRESENT | PTE_WRITE | PTE_PS | global;

    uint32_t cr4 = read_cr4() | (1u << 4);  // PSE
    if (pge) cr4 |= 1u << 7;
    if (smep) cr4 |= 1u << 20;              // kernel may never execute user pages
    write_cr4(cr4);
    write_cr3((uint32_t)kpd);
    // PG on, and WP: the kernel honours read-only pages too, so a
    // stray kernel write to its own code faults instead of patching it.
    write_cr0(read_cr0() | 0x80000000u | (1u << 16));
}

uint32_t as_create(void) {
    uint32_t* pd = (uint32_t*)frame_alloc();
    if (!pd) return 0;
    for (int i = 0; i < KERNEL_PDES; i++) pd[i] = kpd[i];
    return (uint32_t)pd;
}

void as_destroy(uint32_t pdp) {
    uint32_t* pd = (uint32_t*)pdp;
    for (int i = KERNEL_PDES; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)FRAME(pd[i]);
        for (int j = 0; j < 1024; j++)
            if (pt[j] & PTE_PRESENT) frame_free(FRAME(pt[j]));
        frame_free((uint32_t)pt);
    }
    frame_free(pdp);
}

static uint32_t* pte_for(uint32_t pdp, uint32_t va, int create) {
    uint32_t* pd = (uint32_t*)pdp;
    uint32_t* pde = &pd[PDE_INDEX(va)];
    if (!(*pde & PTE_PRESENT)) {
        if (!create) return 0;
        uint32_t pt = frame_alloc();
        if (!pt) return 0;
        *pde = pt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return &((uint32_t*)FRAME(*pde))[PTE_INDEX(va)];
}

uint32_t as_map(uint32_t pd, uint32_t va, int writable) {
    if (va < USER_BASE || va >= USER_TOP || (va & (PAGE_SIZE - 1))) return 0;
    uint32_t* pte = pte_for(pd, va, 1);
    if (!pte) return 0;
    if (*pte & PTE_PRESENT) {
        if (writable && !(*pte & PTE_WRITE)) {
            *pte |= PTE_WRITE;
            if (read_cr3() == pd) invlpg(va);
        }
        return FRAME(*pte);
    }
    uint32_t f = frame_alloc();
    if (!f) return 0;
    *pte = f | PTE_PRESENT | PTE_USER | (writable ? PTE_WRITE : 0);
    return f;
}

uint32_t as_lookup(uint32_t pd, uint32_t va, int* writable) {
    if (va < USER_BASE || va >= USER_TOP) return 0;
    uint32_t* pte = pte_for(pd, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    if (writable) *writable = (*pte & PTE_WRITE) != 0;
    return FRAME(*pte);
}

uint32_t as_pages(uint32_t pdp) {
    uint32_t* pd = (uint32_t*)pdp;
    uint32_t n = 0;
    for (int i = KERNEL_PDES; i < USER_PDE_END; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)FRAME(pd[i]);
        for (int j = 0; j < 1024; j++) n += (pt[j] & PTE_PRESENT) != 0;
    }
    return n;
}

static int in_stack_region(uint32_t addr) {
    return addr >= USER_STACK_LOW && addr < USER_STACK_TOP;
}

int user_fault_fixup(uint32_t addr, uint32_t err) {
    if (err & 1) return 0;  // the page was present: a protection violation
    if (!in_stack_region(addr)) return 0;
    return as_map(current->cr3, ALIGN_DOWN(addr, PAGE_SIZE), 1) != 0;
}

// Validate one page of the current address space for access, growing
// the stack if the page lies in the stack region and is not mapped.
static int page_ok(uint32_t va, int write) {
    int w = 0;
    if (as_lookup(current->cr3, va, &w)) return !write || w;
    if (in_stack_region(va)) return as_map(current->cr3, ALIGN_DOWN(va, PAGE_SIZE), 1) != 0;
    return 0;
}

int user_range_ok(uint32_t uaddr, uint32_t len, int write) {
    if (len == 0) return 0;
    uint32_t end = uaddr + len;
    if (uaddr < USER_BASE || end < uaddr || end > USER_TOP) return -EFAULT;
    for (uint32_t p = ALIGN_DOWN(uaddr, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        if (!page_ok(p, write)) return -EFAULT;
        if (p + PAGE_SIZE < p) break;
    }
    return 0;
}

int copy_from_user(void* dst, const void* usrc, size_t len) {
    if (user_range_ok((uint32_t)usrc, len, 0)) return -EFAULT;
    memcpy(dst, usrc, len);
    return 0;
}

int copy_to_user(void* udst, const void* src, size_t len) {
    if (user_range_ok((uint32_t)udst, len, 1)) return -EFAULT;
    memcpy(udst, src, len);
    return 0;
}

int strncpy_from_user(char* dst, const char* usrc, size_t max) {
    uint32_t a = (uint32_t)usrc;
    for (size_t i = 0; i < max; i++, a++) {
        if (i == 0 || (a & (PAGE_SIZE - 1)) == 0) {
            if (user_range_ok(a, 1, 0)) return -EFAULT;
        }
        dst[i] = *(const char*)a;
        if (!dst[i]) return (int)i;
    }
    if (max) dst[max - 1] = 0;
    return -ENAMETOOLONG;
}

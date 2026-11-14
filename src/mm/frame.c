// Physical frame allocator: a free list threaded through the free
// frames themselves, which the kernel can always reach because all
// managed frames lie inside the identity-mapped first gigabyte.
#include "kernel.h"
#include "arch.h"
#include "mm.h"

static uintptr_t free_head;   // physical address of the first free frame, 0 if none
static uint32_t total_frames;
static uint32_t free_frames;

static void push(uintptr_t phys) {
    *(uintptr_t*)phys_to_virt(phys) = free_head;
    free_head = phys;
    free_frames++;
}

void frames_init(uintptr_t reserved_end) {
    uintptr_t kstart = ALIGN_DOWN(virt_to_phys(_kernel_start), PAGE_SIZE);
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        if (e->type != 1) continue;
        uint64_t lo = e->base, hi = e->base + e->len;
        if (lo < 0x100000) lo = 0x100000;  // leave low memory to the BIOS and VGA
        if (hi > RAM_IDENTITY_LIMIT) hi = RAM_IDENTITY_LIMIT;
        if (lo >= hi) continue;
        uintptr_t a = (uintptr_t)ALIGN_UP(lo, PAGE_SIZE);
        uintptr_t b = (uintptr_t)ALIGN_DOWN(hi, PAGE_SIZE);
        for (uintptr_t p = a; p < b; p += PAGE_SIZE) {
            if (p >= kstart && p < reserved_end) continue;  // kernel image and heap
            push(p);
            total_frames++;
        }
    }
}

uintptr_t frame_alloc(void) {
    uint32_t f = irq_save();
    uintptr_t p = free_head;
    if (p) {
        free_head = *(uintptr_t*)phys_to_virt(p);
        free_frames--;
    }
    irq_restore(f);
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void frame_free(uintptr_t phys) {
    if (!phys || (phys & (PAGE_SIZE - 1))) panic("frame_free(%08lx): bad frame", (unsigned long)phys);
    uint32_t f = irq_save();
    push(phys);
    irq_restore(f);
}

void frame_stats(uint32_t* total, uint32_t* free) {
    *total = total_frames;
    *free = free_frames;
}

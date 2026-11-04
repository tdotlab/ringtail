// Physical frame allocator: a free list threaded through the free
// frames themselves, which the kernel can always reach because all
// managed frames lie inside the identity-mapped first gigabyte.
#include "kernel.h"
#include "arch.h"
#include "mm.h"

static uint32_t free_head;   // physical address of the first free frame, 0 if none
static uint32_t total_frames;
static uint32_t free_frames;

static void push(uint32_t phys) {
    *(uint32_t*)phys = free_head;
    free_head = phys;
    free_frames++;
}

void frames_init(uint32_t reserved_end) {
    uint32_t kstart = ALIGN_DOWN((uint32_t)_kernel_start, PAGE_SIZE);
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        if (e->type != 1) continue;
        uint64_t lo = e->base, hi = e->base + e->len;
        if (lo < 0x100000) lo = 0x100000;  // leave low memory to the BIOS and VGA
        if (hi > RAM_IDENTITY_LIMIT) hi = RAM_IDENTITY_LIMIT;
        if (lo >= hi) continue;
        uint32_t a = ALIGN_UP((uint32_t)lo, PAGE_SIZE);
        uint32_t b = ALIGN_DOWN((uint32_t)hi, PAGE_SIZE);
        for (uint32_t p = a; p < b; p += PAGE_SIZE) {
            if (p >= kstart && p < reserved_end) continue;  // kernel image and heap
            push(p);
            total_frames++;
        }
    }
}

uint32_t frame_alloc(void) {
    uint32_t f = irq_save();
    uint32_t p = free_head;
    if (p) {
        free_head = *(uint32_t*)p;
        free_frames--;
    }
    irq_restore(f);
    if (p) memset((void*)p, 0, PAGE_SIZE);
    return p;
}

void frame_free(uint32_t phys) {
    if (!phys || (phys & (PAGE_SIZE - 1))) panic("frame_free(%08x): bad frame", phys);
    uint32_t f = irq_save();
    push(phys);
    irq_restore(f);
}

void frame_stats(uint32_t* total, uint32_t* free) {
    *total = total_frames;
    *free = free_frames;
}

// Physical frame allocator.
//
// RAM no one has used yet is kept as ranges, from the memory map, and
// handed out a frame at a time; frames that come back go on a free
// list threaded through the frames themselves, and are reused first.
// So a frame is first touched when it is handed out, not at boot,
// which matters with gigabytes: the old allocator wrote into every
// frame of RAM before the shell came up.
//
// Before paging_init has built the direct map, the kernel can reach
// only what the boot page tables map (BOOT_MAP_LIMIT), so frames come
// from the bottom of RAM; paging_init's own tables are the ones taken
// then. After frames_mapped, they come from the top, so RAM above
// 4 GB, when there is any, is in use from the first program on.
// No driver here needs memory below 4 GB (the disk is PIO, and the
// e1000 takes 64-bit addresses); one that did would need its own call.
#include "kernel.h"
#include "arch.h"
#include "mm.h"

typedef struct {
    uintptr_t lo, hi;  // frames not yet handed out, [lo, hi)
} range_t;

static range_t ranges[BOOT_MMAP_MAX + 1];  // +1: the heap can split one
static int nranges;
static int top_down;
static uintptr_t free_head;   // physical address of the first free frame, 0 if none
static uint32_t total_frames;
static uint32_t free_frames;

static void add_range(uintptr_t lo, uintptr_t hi) {
    if (lo >= hi || nranges == (int)(sizeof ranges / sizeof ranges[0])) return;
    // Sorted by address, so the bottom-up phase takes the lowest frames.
    int i = nranges++;
    while (i > 0 && ranges[i - 1].lo > lo) {
        ranges[i] = ranges[i - 1];
        i--;
    }
    ranges[i] = (range_t){lo, hi};
}

void frames_init(uintptr_t reserved_end, uintptr_t limit) {
    uintptr_t kstart = ALIGN_DOWN(virt_to_phys(_kernel_start), PAGE_SIZE);
    reserved_end = ALIGN_UP(reserved_end, PAGE_SIZE);
    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const mmap_entry_t* e = &boot_info.mmap[i];
        if (e->type != 1) continue;
        uint64_t lo = e->base, hi = e->base + e->len;
        if (lo < 0x100000) lo = 0x100000;  // leave low memory to the BIOS and VGA
        if (hi > limit) hi = limit;
        if (lo >= hi) continue;
        uintptr_t a = (uintptr_t)ALIGN_UP(lo, PAGE_SIZE);
        uintptr_t b = (uintptr_t)ALIGN_DOWN(hi, PAGE_SIZE);
        // Without the kernel image and the heap.
        if (a < reserved_end && b > kstart) {
            add_range(a, kstart);
            add_range(reserved_end, b);
        } else {
            add_range(a, b);
        }
    }
    // A memory map may list overlapping regions; give each frame once.
    for (int i = 1; i < nranges; i++) {
        if (ranges[i].lo < ranges[i - 1].hi) ranges[i].lo = ranges[i - 1].hi;
        if (ranges[i].hi < ranges[i].lo) ranges[i].hi = ranges[i].lo;
    }
    for (int i = 0; i < nranges; i++) total_frames += (uint32_t)((ranges[i].hi - ranges[i].lo) / PAGE_SIZE);
    free_frames = total_frames;
}

void frames_mapped(void) {
    top_down = 1;
}

// A frame no one has used yet, or 0. Interrupts are off.
static uintptr_t take_fresh(void) {
    if (top_down) {
        for (int i = nranges - 1; i >= 0; i--)
            if (ranges[i].lo < ranges[i].hi) return ranges[i].hi -= PAGE_SIZE;
        return 0;
    }
    for (int i = 0; i < nranges; i++) {
        if (ranges[i].lo < ranges[i].hi) {
            uintptr_t p = ranges[i].lo;
            if (p + PAGE_SIZE > BOOT_MAP_LIMIT) return 0;
            ranges[i].lo += PAGE_SIZE;
            return p;
        }
    }
    return 0;
}

uintptr_t frame_alloc(void) {
    uint32_t f = irq_save();
    uintptr_t p = free_head;
    if (p) free_head = *(uintptr_t*)phys_to_virt(p);
    else p = take_fresh();
    if (p) free_frames--;
    irq_restore(f);
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void frame_free(uintptr_t phys) {
    if (!phys || (phys & (PAGE_SIZE - 1))) panic("frame_free(%08lx): bad frame", (unsigned long)phys);
    uint32_t f = irq_save();
    *(uintptr_t*)phys_to_virt(phys) = free_head;
    free_head = phys;
    free_frames++;
    irq_restore(f);
}

void frame_stats(uint32_t* total, uint32_t* free) {
    *total = total_frames;
    *free = free_frames;
}

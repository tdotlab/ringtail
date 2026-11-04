// Now kfree actually works, memory can be reused

#include <stdint.h>
#include <stddef.h>

extern char _kernel_end[];

// v0.10.4: the heap size is no longer a constant. HEAP_SIZE
// used to be 4 MB, fixed, at a fixed spot after the kernel,
// which worked because QEMU's RAM layout is known and
// forgiving, but it assumed RAM instead of asking. Now the
// size is a variable, set by heap_init_at from the
// multiboot memory map (see heap_init_from_map), and the
// old 4 MB survives only as the host-test heap size, via
// heap_init. The allocator, the free list, and kmalloc/kfree
// are unchanged: only where the heap lives, and how big it
// is, moved from a constant to a map-derived value.
#define HOST_HEAP_SIZE (4 * 1024 * 1024)
#define ALIGNMENT 8
// Heap placement bounds, v0.10.4. The heap must stay below
// 16 MB, because paging identity-maps only 0-16 MB, and
// it must leave room for page frames above it, because
// paging_init builds its frame pool just above heap_end.
// FRAME_RESERVE is that room: 1 MB, or 256 frames, far
// more than the handful of concurrent tasks a toy OS
// ever holds (kernel tables need 5 frames, a user task
// needs 2), while still giving the heap almost everything
// else. The reserve is subtracted from the region end
// in every case, not just capped at 15 MB: capping
// alone would leave a small-RAM machine with only the
// gap between the usable region's end and RAM top for
// frames (128 KB in QEMU, zero on hardware whose
// usable region ends exactly at RAM top, which would
// make paging_init halt with no frames). Subtracting
// guarantees at least 1 MB of frames on any machine
// that boots, and on a large-RAM machine the heap
// still ends at 15 MB (16 MB minus the reserve),
// exactly as a cap would give.
// MIN_HEAP_SIZE is the smallest heap worth booting with:
// 1 MB covers disk buffers (about 82 KB), task stacks
// (4 KB each), and ordinary allocations with room to
// spare, and anything smaller halts honestly instead of
// booting into a heap that cannot hold its own buffers.
#define FRAME_RESERVE (1 * 1024 * 1024)
#define IDENTITY_LIMIT (16 * 1024 * 1024)
#define MIN_HEAP_SIZE (1 * 1024 * 1024)

typedef struct block {
    uint32_t size; // usable size after header
    struct block* next;
    uint8_t free;
    uint8_t _pad[3];
    uint32_t reserved; // pad header to 16 bytes: keeps every block and
                       // every data pointer 8-byte aligned after a split
} block_t;

static uint8_t* heap_start = 0;
static uint8_t* heap_end = 0;
static uint32_t heap_size = 0;
static block_t* free_list = 0;
static uint32_t heap_used = 0;

// v0.5.1: interrupt save/restore for heap operations. A heap
// operation (split, coalesce, free-list walk) must not be
// preempted in the middle, or another task's heap operation could
// see a half-updated list and corrupt it. Disabling interrupts
// for the operation prevents the timer from switching tasks
// mid-operation. The save/restore form matters: it nests safely.
// An outer save records interrupts on, an inner save (from a
// ramfs function that already disabled them, or from kfree called
// inside a locked heap walk) records them off and restores them
// off, so only the outermost restore re-enables them. A bare
// cli/sti pair would re-enable too early on the inner restore.
// The guard is load-bearing: cli/sti are privileged instructions
// that fault in user mode, so host tests must NOT compile them
// in. Kernel builds define RINGTAIL_KERNEL (see the Makefile);
// host test builds do not, and get no-op helpers instead, which
// is correct there because host tests are single-threaded and
// nothing preempts them. If a kernel build ever forgets the
// define, heap locking silently becomes a no-op, so the define
// is part of the build, not an optional extra.
#ifdef RINGTAIL_KERNEL
static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    return flags;
}
static void irq_restore(uint32_t flags) {
    if (flags & 0x200) __asm__ volatile("sti");
}
#else
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
#endif

static uint32_t align_up(uint32_t addr, uint32_t align) {
    return (addr + align - 1) & ~(align - 1);
}

// General heap init: place the heap at start, with size
// bytes, as one free block. The caller guarantees size is
// larger than a block header; heap_init and
// heap_init_from_map both do, so this takes no error path,
// like the old heap_init. Start is expected page-aligned
// by the caller (kernel_end is aligned here, host tests
// align their array the same way); the free list itself
// needs only 8-byte alignment, which a page-aligned start
// already gives.
void heap_init_at(uint32_t start, uint32_t size) {
    heap_start = (uint8_t*)start;
    heap_end = heap_start + size;
    heap_size = size;

    // first block covers whole heap minus header
    free_list = (block_t*)heap_start;
    free_list->size = size - sizeof(block_t);
    free_list->next = NULL;
    free_list->free = 1;
    heap_used = 0;
}

void heap_init(void) {
    // Host-test heap, unchanged in behavior: 4 MB at the
    // test's _kernel_end array, page-aligned. Kernel boot
    // no longer calls this; it calls heap_init_from_map
    // instead, so the kernel heap is map-derived while
    // host tests keep the fixed 4 MB heap their CHECKs
    // expect (total 4 MB, initial free 4 MB minus header).
    uint32_t start = (uint32_t)_kernel_end;
    start = align_up(start, 4096);
    heap_init_at(start, HOST_HEAP_SIZE);
}

#ifdef RINGTAIL_KERNEL
// Multiboot memory map, stored for the shell's `mmap`
// command. Set once by heap_init_from_map at boot, never
// changed after: the info struct itself may be
// overwritten by the heap after boot, so re-reading
// info[11]/info[12] later would give heap contents,
// not the map address, and the shell must use these
// saved values instead. The map array itself is still
// read from its original GRUB address by mmap_dump
// (see there for the limit that carries).
static uint32_t saved_mmap_addr = 0;
static uint32_t saved_mmap_length = 0;

// Heap placement from the multiboot memory map, v0.10.4.
// The kernel_main caller passes mbi, the multiboot info
// pointer from boot.asm (second stack argument, from
// ebx). Layout, in 32-bit words: info[0] flags,
// info[11] mmap_length in bytes, info[12] mmap_addr,
// a pointer to the first entry. Each entry starts with
// a 4-byte size field (bytes following it, typically
// 20), then base address (8 bytes, 64-bit), length
// (8 bytes), and type (4 bytes, 1 = usable RAM).
// Entries are walked by size + 4, not by a fixed
// stride, because the spec allows larger entries.
// Only type 1 entries are usable; everything else
// (reserved, ACPI, bad) is skipped, conservatively:
// ACPI reclaimable could be reused after ACPI init,
// but RingTail has no ACPI, so it stays reserved.
// Only entries below 4 GB are considered: RingTail
// is 32-bit with no PAE, and a base or length with a
// nonzero high word cannot be represented in a
// 32-bit pointer or intersected with the 16 MB
// identity range without 64-bit arithmetic, so those
// entries are skipped, not misread. This is a stated
// 32-bit limit, not a QEMU limit: QEMU's map is all
// below 4 GB, so nothing is skipped in practice.
// The heap goes in the LARGEST usable region that
// overlaps the kernel (that is, whose end is above
// kernel_end), intersected with the identity range
// minus the frame reserve: start is the later of the
// region start and page-aligned kernel_end, end is
// the earlier of the region end and 16 MB, minus
// FRAME_RESERVE (so on a large-RAM machine the heap
// ends at 15 MB). Largest wins, because usable RAM below
// 16 MB may be fragmented (low memory, then a
// reserved hole, then RAM above 1 MB), and the heap
// should take the biggest usable piece, not the
// first. Smaller pieces and gaps are wasted in this
// slice: a full physical memory manager that used
// every region is a later piece, and QEMU's map is
// contiguous above 1 MB, so nothing is wasted there.
// Boot info is single-use, and that is what makes
// overwriting it safe, with one exception: kernel_main
// already read mem_upper before this runs, this
// function reads the whole map before placing the
// heap, and nothing after heap_init reads the info
// struct, the command line, or the modules again, so
// the heap writing over those addresses afterwards
// destroys nothing the OS still needs, except for
// the shell's mmap_dump, which reads the map array
// again from the saved address (see mmap_dump for
// the limit that carries, and why placement itself
// is unaffected even if a later dump were corrupt).
// Failure
// halts with a diagnostic, like paging_init's
// out-of-frames halt: a heap is essential, and
// booting without one, or with a silently wrong
// one, would be worse than stopping with a message.
// No mmap, no usable region, or a region too small
// for MIN_HEAP_SIZE all halt here, before paging,
// so the diagnostic prints while paging is still
// off and VGA text still works.
void heap_init_from_map(uint32_t mbi) {
    extern void print(const char* s);
    if (mbi == 0) {
        print("No multiboot info for heap - halting.\n");
        while (1) __asm__ volatile("hlt");
    }
    uint32_t* info = (uint32_t*)mbi;
    if ((info[0] & 0x40) == 0) {
        print("No memory map for heap - halting.\n");
        while (1) __asm__ volatile("hlt");
    }
    uint32_t mmap_addr = info[12];
    uint32_t mmap_length = info[11];
    saved_mmap_addr = mmap_addr;
    saved_mmap_length = mmap_length;
    uint32_t kernel_start = align_up((uint32_t)_kernel_end, 4096);
    uint32_t best_start = 0;
    uint32_t best_size = 0;
    uint32_t ptr = mmap_addr;
    uint32_t map_end = mmap_addr + mmap_length;
    while (ptr < map_end) {
        uint32_t entry_size = *(uint32_t*)ptr;
        uint32_t base_low = *(uint32_t*)(ptr + 4);
        uint32_t base_high = *(uint32_t*)(ptr + 8);
        uint32_t len_low = *(uint32_t*)(ptr + 12);
        uint32_t len_high = *(uint32_t*)(ptr + 16);
        uint32_t type = *(uint32_t*)(ptr + 20);
        // Stride first, so a skipped entry still advances
        // correctly, even if its fields are unusable.
        ptr += entry_size + 4;
        if (type != 1) continue;
        if (base_high != 0 || len_high != 0) continue;
        uint32_t region_end = base_low + len_low;
        // Overflow guard: a base + length that wraps
        // past 4 GB would look like a tiny region, not
        // a huge one, so reject wrapped ends outright.
        if (region_end < base_low) continue;
        uint32_t start = base_low > kernel_start ? base_low : kernel_start;
        // Cap at the identity limit, then subtract the
        // frame reserve, in that order, so the reserve
        // holds on small RAM as well as large (see the
        // bounds comment above). A capped end at or
        // below the reserve leaves nothing for the heap
        // in this region, so it is skipped, not given
        // a wrapped or zero size.
        uint32_t capped = region_end < IDENTITY_LIMIT ? region_end : IDENTITY_LIMIT;
        if (capped <= FRAME_RESERVE) continue;
        uint32_t end = capped - FRAME_RESERVE;
        if (end <= start) continue;
        uint32_t size = end - start;
        if (size > best_size) {
            best_size = size;
            best_start = start;
        }
    }
    if (best_size < MIN_HEAP_SIZE) {
        print("Not enough usable memory for heap - halting.\n");
        while (1) __asm__ volatile("hlt");
    }
    heap_init_at(best_start, best_size);
}

static void mmap_print_hex32(uint32_t n) {
    extern void print(const char* s);
    extern void print_char(char c);
    const char* digits = "0123456789ABCDEF";
    print("0x");
    for (int i = 7; i >= 0; i--) print_char(digits[(n >> (i * 4)) & 0xF]);
}

// Print the saved multiboot memory map, one line per
// entry, for the shell's `mmap` command. This is the
// receipt for heap placement: `heap` shows where the
// heap landed, `mmap` shows the map it was placed
// from, so the two can be checked against each other
// instead of taking placement on trust. The address
// and length are the ones saved at boot, not re-read
// from the info struct, because the heap may have
// overwritten the info struct itself by now. The map
// array is still read from its original GRUB address,
// which survives heap placement in QEMU (proven by
// the correct dump in testing), but a bootloader
// that placed the map array inside the chosen heap
// region would corrupt a later dump: placement
// itself is unaffected, because placement reads the
// whole map before writing any heap, but the dump
// would then print heap contents as if they were a
// map. Copying the map to a safe buffer at boot is
// a future hardening, not done in this slice. If
// heap_init_from_map never ran (which cannot happen
// on a booted kernel, but keeps the function total),
// it prints nothing but the header.
void mmap_dump(void) {
    extern void print(const char* s);
    print("Memory map:\n");
    if (saved_mmap_length == 0) return;
    uint32_t ptr = saved_mmap_addr;
    uint32_t map_end = saved_mmap_addr + saved_mmap_length;
    while (ptr < map_end) {
        uint32_t entry_size = *(uint32_t*)ptr;
        uint32_t base_low = *(uint32_t*)(ptr + 4);
        uint32_t base_high = *(uint32_t*)(ptr + 8);
        uint32_t len_low = *(uint32_t*)(ptr + 12);
        uint32_t len_high = *(uint32_t*)(ptr + 16);
        uint32_t type = *(uint32_t*)(ptr + 20);
        ptr += entry_size + 4;
        print("  base=");
        // 64-bit base/length are printed as high:low
        // pairs when the high word is nonzero, so a
        // skipped above-4GB entry still shows honestly
        // instead of silently printing its low word.
        if (base_high != 0) {
            mmap_print_hex32(base_high);
            print(":");
        }
        mmap_print_hex32(base_low);
        print(" len=");
        if (len_high != 0) {
            mmap_print_hex32(len_high);
            print(":");
        }
        mmap_print_hex32(len_low);
        print(" type=");
        // Small decimal print, local, like other files.
        char buf[12];
        int i = 0;
        if (type == 0) { print("0"); }
        else {
            uint32_t t = type;
            while (t > 0) { buf[i++] = (char)('0' + (t % 10)); t /= 10; }
            char out[12];
            int j = 0;
            while (i > 0) out[j++] = buf[--i];
            out[j] = 0;
            print(out);
        }
        if (type == 1) print(" usable\n");
        else print(" reserved\n");
    }
}
#endif


static block_t* find_free(uint32_t size) {
    block_t* curr = free_list;
    while (curr) {
        if (curr->free && curr->size >= size) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static void split_block(block_t* block, uint32_t size) {
    // if remaining space is enough for new block header + minimal size
    if (block->size >= size + sizeof(block_t) + ALIGNMENT) {
        uint8_t* block_data = (uint8_t*)(block + 1);
        block_t* new_block = (block_t*)(block_data + size);
        new_block->size = block->size - size - sizeof(block_t);
        new_block->free = 1;
        new_block->next = block->next;

        block->size = size;
        block->next = new_block;
    }
}

// The real allocator, with interrupts already off (see kmalloc
// below). Keeping the lock in the wrapper, not in the body, means
// every return path is covered by construction: the body can
// return early as it always did, and the wrapper restores.
static void* kmalloc_impl(uint32_t size) {
    if (size == 0) return NULL;
    // Overflow guards (ChatGPT review, v0.2.3): align_up adds 7
    // before masking, so a size near 2^32 wrapped to a tiny value
    // and kmalloc happily returned a small block for a huge request.
    // Reject anything that cannot fit in the heap at all, and
    // anything whose alignment arithmetic would wrap, before aligning.
    // v0.10.4: the bound is heap_size, the map-derived size,
    // not the old 4 MB constant, so a larger heap actually
    // allows larger allocations, and a smaller heap rejects
    // sooner. Before heap_init, heap_size is 0 and find_free
    // returns NULL anyway, so a pre-init kmalloc still fails
    // safely, as before.
    if (heap_size <= sizeof(block_t)) return NULL;
    if (size > heap_size - sizeof(block_t)) return NULL;
    if (size > 0xFFFFFFFFu - (ALIGNMENT - 1)) return NULL;
    size = align_up(size, ALIGNMENT);
    if (size > heap_size - sizeof(block_t)) return NULL;

    block_t* block = find_free(size);
    if (!block) return NULL;

    split_block(block, size);
    block->free = 0;
    heap_used += block->size + sizeof(block_t);

    return (void*)(block + 1);
}

void* kmalloc(uint32_t size) {
    uint32_t flags = irq_save();
    void* p = kmalloc_impl(size);
    irq_restore(flags);
    return p;
}

static int kfree_impl(void* ptr) {
    if (!ptr) return -1;
    // Find the block in the list first; never dereference ptr-1 until
    // we know it really is a block header in this heap.
    block_t* target = ((block_t*)ptr) - 1;
    block_t* curr = free_list;
    while (curr) {
        if (curr == target) break;
        curr = curr->next;
    }
    if (!curr) return -1; // not a heap allocation at all
    if (curr->free) return -2; // already freed

    curr->free = 1;
    heap_used -= curr->size + sizeof(block_t);

    // coalesce adjacent free blocks
    curr = free_list;
    while (curr && curr->next) {
        uint8_t* curr_end = (uint8_t*)curr + sizeof(block_t) + curr->size;
        if (curr->free && curr->next->free && curr_end == (uint8_t*)curr->next) {
            // merge next into curr
            curr->size += sizeof(block_t) + curr->next->size;
            curr->next = curr->next->next;
            continue; // check again with new next
        }
        curr = curr->next;
    }
    return 0;
}

int kfree(void* ptr) {
    uint32_t flags = irq_save();
    int r = kfree_impl(ptr);
    irq_restore(flags);
    return r;
}

uint32_t heap_get_used(void) { return heap_used; }
uint32_t heap_get_total(void) { return heap_size; }

uint32_t heap_get_free(void) {
    // Walk under the same protection: a concurrent split or
    // coalesce changing next pointers mid-walk would give a
    // wrong total, and in the worst case a stale walk. The
    // single-value getters (used, total, start, end) need no
    // lock: a 32-bit aligned load is atomic on x86, and total,
    // start, and end never change after heap_init.
    uint32_t flags = irq_save();
    uint32_t free = 0;
    block_t* curr = free_list;
    while (curr) {
        if (curr->free) free += curr->size;
        curr = curr->next;
    }
    irq_restore(flags);
    return free;
}

uint32_t heap_get_start(void) { return (uint32_t)heap_start; }
uint32_t heap_get_end(void) { return (uint32_t)heap_end; }

// debug: count free blocks
uint32_t heap_get_free_blocks(void) {
    uint32_t flags = irq_save();
    uint32_t count = 0;
    block_t* curr = free_list;
    while (curr) {
        if (curr->free) count++;
        curr = curr->next;
    }
    irq_restore(flags);
    return count;
}

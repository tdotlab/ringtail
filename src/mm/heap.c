// Now kfree actually works, memory can be reused

#include <stdint.h>
#include <stddef.h>

extern char _kernel_end[];

#define HEAP_SIZE (4 * 1024 * 1024)
#define ALIGNMENT 8

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

void heap_init(void) {
    uint32_t start = (uint32_t)_kernel_end;
    start = align_up(start, 4096);
    heap_start = (uint8_t*)start;
    heap_end = heap_start + HEAP_SIZE;

    // first block covers whole heap minus header
    free_list = (block_t*)heap_start;
    free_list->size = HEAP_SIZE - sizeof(block_t);
    free_list->next = NULL;
    free_list->free = 1;
    heap_used = 0;
}

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
    if (size > HEAP_SIZE - sizeof(block_t)) return NULL;
    if (size > 0xFFFFFFFFu - (ALIGNMENT - 1)) return NULL;
    size = align_up(size, ALIGNMENT);
    if (size > HEAP_SIZE - sizeof(block_t)) return NULL;

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
uint32_t heap_get_total(void) { return HEAP_SIZE; }

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

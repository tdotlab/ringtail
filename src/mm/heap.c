// Kernel heap: first-fit over one address-ordered list of blocks,
// each with a 16-byte header, merged with free neighbours on free.
// Every allocation is 16-byte aligned.
//
// kfree validates its argument by finding it in the block list, so
// a wild pointer or a double free is reported instead of corrupting
// the heap. That makes free O(n), which is fine at this scale.
#include "mm.h"
#include "lib.h"
#ifdef RINGTAIL_KERNEL
#include "arch.h"
#else
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
#endif

#define ALIGN 16

typedef struct block {
    uint32_t size;         // usable bytes after the header
    struct block* next;    // next block in address order
    uint32_t free;
    uint32_t magic;
} block_t;

#define BLOCK_MAGIC 0x4B484550u

static uint8_t* heap_base;
static uint32_t heap_size;
static block_t* blocks;
static uint32_t heap_used;

void heap_init(void* start, size_t size) {
    uintptr_t s = ALIGN_UP((uintptr_t)start, ALIGN);
    size -= s - (uintptr_t)start;
    size = ALIGN_DOWN(size, ALIGN);
    heap_base = (uint8_t*)s;
    heap_size = (uint32_t)size;
    blocks = (block_t*)heap_base;
    blocks->size = heap_size - sizeof(block_t);
    blocks->next = 0;
    blocks->free = 1;
    blocks->magic = BLOCK_MAGIC;
    heap_used = 0;
}

void* kmalloc(size_t size) {
    if (size == 0 || size > heap_size) return 0;
    size = ALIGN_UP(size, ALIGN);
    uint32_t f = irq_save();
    block_t* b = blocks;
    while (b && !(b->free && b->size >= size)) b = b->next;
    if (!b) { irq_restore(f); return 0; }
    // Split when the remainder can hold a header and some data.
    if (b->size >= size + sizeof(block_t) + ALIGN) {
        block_t* rest = (block_t*)((uint8_t*)(b + 1) + size);
        rest->size = b->size - (uint32_t)size - sizeof(block_t);
        rest->next = b->next;
        rest->free = 1;
        rest->magic = BLOCK_MAGIC;
        b->size = (uint32_t)size;
        b->next = rest;
    }
    b->free = 0;
    heap_used += b->size + sizeof(block_t);
    irq_restore(f);
    return b + 1;
}

void* kzalloc(size_t size) {
    void* p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static int adjacent(block_t* a, block_t* b) {
    return (uint8_t*)(a + 1) + a->size == (uint8_t*)b;
}

int kfree(void* p) {
    if (!p) return -1;
    block_t* target = (block_t*)p - 1;
    uint32_t f = irq_save();
    block_t* prev = 0;
    block_t* b = blocks;
    while (b && b != target) { prev = b; b = b->next; }
    if (!b || b->magic != BLOCK_MAGIC) { irq_restore(f); return -1; }
    if (b->free) { irq_restore(f); return -2; }
    b->free = 1;
    heap_used -= b->size + sizeof(block_t);
    if (b->next && b->next->free && adjacent(b, b->next)) {
        b->size += sizeof(block_t) + b->next->size;
        b->next->magic = 0;
        b->next = b->next->next;
    }
    if (prev && prev->free && adjacent(prev, b)) {
        prev->size += sizeof(block_t) + b->size;
        prev->next = b->next;
        b->magic = 0;
    }
    irq_restore(f);
    return 0;
}

void heap_get_stats(heap_stats_t* s) {
    uint32_t f = irq_save();
    s->start = (uint32_t)(uintptr_t)heap_base;
    s->total = heap_size;
    s->used = heap_used;
    s->free = 0;
    s->free_blocks = 0;
    s->largest_free = 0;
    for (block_t* b = blocks; b; b = b->next) {
        if (!b->free) continue;
        s->free += b->size;
        s->free_blocks++;
        if (b->size > s->largest_free) s->largest_free = b->size;
    }
    irq_restore(f);
}

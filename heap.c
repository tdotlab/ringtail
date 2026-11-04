// RingTail - v0.1.1 Piece 9: Heap with free list + coalescing
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

void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    size = align_up(size, ALIGNMENT);

    block_t* block = find_free(size);
    if (!block) return NULL;

    split_block(block, size);
    block->free = 0;
    heap_used += block->size + sizeof(block_t);

    return (void*)(block + 1);
}

int kfree(void* ptr) {
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

uint32_t heap_get_used(void) { return heap_used; }
uint32_t heap_get_total(void) { return HEAP_SIZE; }

uint32_t heap_get_free(void) {
    uint32_t free = 0;
    block_t* curr = free_list;
    while (curr) {
        if (curr->free) free += curr->size;
        curr = curr->next;
    }
    return free;
}

uint32_t heap_get_start(void) { return (uint32_t)heap_start; }
uint32_t heap_get_end(void) { return (uint32_t)heap_end; }

// debug: count free blocks
uint32_t heap_get_free_blocks(void) {
    uint32_t count = 0;
    block_t* curr = free_list;
    while (curr) {
        if (curr->free) count++;
        curr = curr->next;
    }
    return count;
}

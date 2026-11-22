// The arena: see arena.h.
#include "arena.h"
#include <string.h>

#define CHUNK_SIZE (64 * 1024)  // data bytes in an ordinary chunk
#define BIG (CHUNK_SIZE / 4)    // a request this big gets a chunk of its own

struct arena_chunk {
    arena_chunk_t* next;
    size_t cap, used;
    uint64_t pad_to_32;  // so the data that follows is 16-byte aligned
};

#define HDR ((sizeof(arena_chunk_t) + 15) & ~(size_t)15)

void arena_init(arena_t* a, const mem_t* mem) {
    a->mem = mem;
    a->head = 0;
    a->failed = 0;
    a->bytes = 0;
}

static arena_chunk_t* new_chunk(arena_t* a, size_t data) {
    arena_chunk_t* c = a->mem->alloc(HDR + data);
    if (!c) return 0;
    c->next = 0;
    c->cap = data;
    c->used = 0;
    return c;
}

void* arena_alloc(arena_t* a, size_t n) {
    if (n == 0) n = 1;
    if (n > ((size_t)1 << 40)) { a->failed = 1; return 0; }  // nothing sane asks for this
    n = (n + 15) & ~(size_t)15;

    if (n > BIG) {
        // A chunk of its own, linked behind the head so the head's
        // remaining space is still used for small requests.
        arena_chunk_t* c = new_chunk(a, n);
        if (!c) { a->failed = 1; return 0; }
        if (a->head) { c->next = a->head->next; a->head->next = c; }
        else a->head = c;
        c->used = n;
        a->bytes += n;
        memset((uint8_t*)c + HDR, 0, n);
        return (uint8_t*)c + HDR;
    }
    if (!a->head || a->head->cap - a->head->used < n) {
        arena_chunk_t* c = new_chunk(a, CHUNK_SIZE);
        if (!c) { a->failed = 1; return 0; }
        c->next = a->head;
        a->head = c;
    }
    uint8_t* p = (uint8_t*)a->head + HDR + a->head->used;
    a->head->used += n;
    a->bytes += n;
    memset(p, 0, n);
    return p;
}

void arena_free_all(arena_t* a) {
    arena_chunk_t* c = a->head;
    while (c) {
        arena_chunk_t* next = c->next;
        a->mem->release(c);
        c = next;
    }
    a->head = 0;
    a->failed = 0;
    a->bytes = 0;
}

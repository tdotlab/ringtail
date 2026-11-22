// Memory for the core: a chunked bump arena over an allocator the platform gives us.
//
// The core never calls malloc. The platform hands it a mem_t, and every
// allocation comes out of an arena built on it. An arena is released all
// at once, so the core has no free() to forget: the session arena holds
// what lives as long as the conversation, and the scratch arena holds one
// step's work and is emptied at the start of the next step.
//
// Allocation can fail. Rather than make every call site check, a failed
// allocation returns 0, sets `failed` and keeps it set, and the string
// buffer and JSON writer built on the arena turn into no-ops once it is
// set. Whoever finishes a piece of work checks `failed` once.
#ifndef BARB_ARENA_H
#define BARB_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    void* (*alloc)(size_t n);  // 0 on failure
    void (*release)(void* p);
} mem_t;

typedef struct arena_chunk arena_chunk_t;

typedef struct {
    const mem_t* mem;
    arena_chunk_t* head;  // the chunk bumped from; others are only kept to be released
    int failed;           // some allocation failed since arena_init or arena_free_all
    size_t bytes;         // handed out so far, rounded up to 16
} arena_t;

void arena_init(arena_t* a, const mem_t* mem);

// n bytes, zeroed, 16-byte aligned. 0 on failure (and a->failed is set).
void* arena_alloc(arena_t* a, size_t n);

// Release every chunk. The arena can be used again and `failed` is cleared.
void arena_free_all(arena_t* a);

#endif

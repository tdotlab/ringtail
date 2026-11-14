// malloc for RingTail programs, on top of the program break.
//
// The heap is one run of blocks from the first break upward. Each
// block has a 16-byte header with its own size and its lower
// neighbour's, so free can merge with both neighbours at once. malloc
// is first fit; when nothing fits, it grows the heap with sbrk, at
// least 64 KB at a time, extending the last block if that one is free.
// Payloads are 16-byte aligned. A bad free is a bug in the program,
// so it ends the program with a message rather than corrupt the heap.
#include "ulib.h"

typedef struct {
    uint32_t size;       // payload bytes, a multiple of 16
    uint32_t prev_size;  // payload bytes of the block below, 0 for the first
    uint32_t free;
    uint32_t magic;
} hdr_t;

#define MAGIC 0x4D414C43u
#define HDR ((uint32_t)sizeof(hdr_t))
#define GROW_MIN (64u * 1024u)

static uint8_t* heap_lo;
static uint8_t* heap_hi;
static hdr_t* last;

static hdr_t* next_of(hdr_t* h) {
    uint8_t* n = (uint8_t*)(h + 1) + h->size;
    return n < heap_hi ? (hdr_t*)n : 0;
}

static hdr_t* prev_of(hdr_t* h) {
    if ((uint8_t*)h == heap_lo) return 0;
    return (hdr_t*)((uint8_t*)h - h->prev_size - HDR);
}

static void set_next_prev(hdr_t* h) {
    hdr_t* n = next_of(h);
    if (n) n->prev_size = h->size;
    else last = h;
}

// Cut h down to n bytes when the rest can hold another block.
static void split(hdr_t* h, uint32_t n) {
    if (h->size < n + HDR + 16) return;
    hdr_t* r = (hdr_t*)((uint8_t*)(h + 1) + n);
    r->size = h->size - n - HDR;
    r->prev_size = n;
    r->free = 1;
    r->magic = MAGIC;
    h->size = n;
    set_next_prev(r);
}

static void merge_up(hdr_t* h) {
    hdr_t* n = next_of(h);
    if (!n || !n->free) return;
    h->size += HDR + n->size;
    n->magic = 0;
    set_next_prev(h);
}

void* sbrk(int incr) {
    long r = SC1(SYS_SBRK, incr);
    return IS_ERR(r) ? 0 : (void*)r;
}

static int grow(uint32_t bytes) {
    if (bytes < GROW_MIN) bytes = GROW_MIN;
    if (bytes > 0x7FFFFFF0u) return 0;
    uint8_t* p = sbrk((int)bytes);
    if (!p) return 0;
    if (!heap_lo) heap_lo = heap_hi = p;
    if (p != heap_hi) return 0;  // someone else moved the break
    if (last && last->free) {
        last->size += bytes;
    } else {
        hdr_t* h = (hdr_t*)p;
        h->size = bytes - HDR;
        h->prev_size = last ? last->size : 0;
        h->free = 1;
        h->magic = MAGIC;
        last = h;
    }
    heap_hi = p + bytes;
    return 1;
}

static uint32_t round_size(size_t n) {
    if (n > 0x7FFF0000u) return 0;
    return n ? (uint32_t)ALIGN_UP(n, 16) : 16;
}

void* malloc(size_t size) {
    uint32_t n = round_size(size);
    if (!n) return 0;
    for (;;) {
        for (hdr_t* h = heap_lo ? (hdr_t*)heap_lo : 0; h; h = next_of(h)) {
            if (h->free && h->size >= n) {
                split(h, n);
                h->free = 0;
                return h + 1;
            }
        }
        uint32_t need = n + HDR;
        if (last && last->free) need = n - last->size;
        if (!grow(ALIGN_UP(need, 16))) return 0;
    }
}

static hdr_t* checked(void* p, const char* who) {
    hdr_t* h = (hdr_t*)p - 1;
    if ((uint8_t*)h < heap_lo || (uint8_t*)h >= heap_hi || ((uintptr_t)p & 15) ||
        h->magic != MAGIC || h->free) {
        printf("%s: bad pointer %p\n", who, p);
        sys_exit(-3);
    }
    return h;
}

void free(void* p) {
    if (!p) return;
    hdr_t* h = checked(p, "free");
    h->free = 1;
    merge_up(h);
    hdr_t* prev = prev_of(h);
    if (prev && prev->free) merge_up(prev);
}

void* calloc(size_t count, size_t size) {
    if (size && count > 0xFFFFFFFFu / size) return 0;
    void* p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

void* realloc(void* p, size_t size) {
    if (!p) return malloc(size);
    if (!size) { free(p); return 0; }
    hdr_t* h = checked(p, "realloc");
    uint32_t n = round_size(size);
    if (!n) return 0;
    if (h->size < n) {
        hdr_t* nx = next_of(h);
        if (nx && nx->free && h->size + HDR + nx->size >= n) merge_up(h);
    }
    if (h->size >= n) {
        split(h, n);
        hdr_t* r = next_of(h);
        if (r && r->free) merge_up(r);  // a split-off tail joins free space after it
        return p;
    }
    void* q = malloc(size);
    if (!q) return 0;
    memcpy(q, p, h->size);
    free(p);
    return q;
}

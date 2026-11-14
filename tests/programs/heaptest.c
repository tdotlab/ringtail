// Self-checking test of sbrk and malloc, calloc, realloc and free.
#include "ulib.h"

static int pattern_ok(const uint8_t* p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i)) return 0;
    return 1;
}

static void fill(uint8_t* p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i);
}

int main(void) {
    uint8_t* b0 = sbrk(0);
    check(b0 != 0 && ((uintptr_t)b0 & 4095) == 0, "the first break is page aligned");
    uint8_t* b1 = sbrk(100);
    check(b1 == b0 && sbrk(0) == b0 + 100, "sbrk returns the old break");
    b1[0] = 1; b1[99] = 2;  // must be mapped
    check(sbrk(-100) == b0 + 100 && sbrk(0) == b0, "sbrk can shrink");
    check(sbrk(-4096) == 0, "the break cannot go below its start");
    check(sbrk(0x7FFFFFFF) == 0, "a huge sbrk fails");

    void* z = malloc(0);
    check(z != 0, "malloc(0) gives a pointer");
    free(z);

    uint8_t* a = malloc(100);
    uint8_t* b = malloc(5000);
    uint8_t* c = malloc(30);
    check(a && b && c, "three mallocs");
    check(((uintptr_t)a & 15) == 0 && ((uintptr_t)b & 15) == 0 && ((uintptr_t)c & 15) == 0,
          "pointers are 16-byte aligned");
    check(b >= a + 100 && c >= b + 5000, "blocks do not overlap");
    fill(a, 100, 1); fill(b, 5000, 2); fill(c, 30, 3);
    check(pattern_ok(a, 100, 1) && pattern_ok(b, 5000, 2) && pattern_ok(c, 30, 3),
          "contents survive neighbours");

    free(b);
    uint8_t* d = malloc(4000);
    check(d == b, "a freed block is reused");
    free(d);
    free(a);
    uint8_t* e = malloc(5000);
    check(e == a, "freed neighbours merge");
    free(e);

    uint8_t* r = malloc(40);
    fill(r, 40, 9);
    uint8_t* r2 = realloc(r, 20000);
    check(r2 && pattern_ok(r2, 40, 9), "realloc keeps the contents");
    uint8_t* r3 = realloc(r2, 16);
    check(r3 == r2 && pattern_ok(r3, 16, 9), "shrinking realloc stays in place");
    free(r3);

    uint32_t* zeroed = calloc(1000, 4);
    int all_zero = zeroed != 0;
    for (int i = 0; all_zero && i < 1000; i++) all_zero = zeroed[i] == 0;
    check(all_zero, "calloc clears");
    free(zeroed);
    check(calloc(0x10000, 0x10000) == 0, "calloc catches overflow");

    // Grow well past the first sbrk chunk.
    void* big[16];
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        big[i] = malloc(100000);
        if (!big[i]) { ok = 0; break; }
        memset(big[i], i, 100000);
    }
    check(ok, "1.6 MB of allocations");
    for (int i = 0; ok && i < 16; i++) {
        uint8_t* p = big[i];
        if (p[0] != i || p[99999] != i) ok = 0;
        free(p);
    }
    check(ok, "and their contents");
    check(malloc(0x7FFFFFFF) == 0, "an impossible malloc returns NULL");
    free(c);
    return check_summary("heaptest");
}

// Two helpers for code that handles secrets.
#ifndef RINGTAIL_CT_H
#define RINGTAIL_CT_H

#include <stdint.h>
#include <stddef.h>

// Whether two buffers are equal, taking the same time wherever they
// first differ (memcmp stops at the first difference, which tells an
// attacker how much of a forged checksum was right).
static inline int ct_equal(const void* a, const void* b, size_t n) {
    const volatile uint8_t* x = a;
    const volatile uint8_t* y = b;
    uint32_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint32_t)(x[i] ^ y[i]);
    return (int)(1 & ((diff - 1) >> 8));
}

// Overwrites a secret that is no longer needed. Through a volatile
// pointer, so the compiler may not drop the writes as dead.
static inline void ct_wipe(void* p, size_t n) {
    volatile uint8_t* v = p;
    while (n--) *v++ = 0;
}

#endif

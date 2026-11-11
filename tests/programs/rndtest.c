// SYS_RANDOM: the calls work and refuse what they should, and the bytes
// at least look random. (No test can show they are unpredictable; these
// would catch a generator that is stuck, repeats itself or leans to one
// side.)
#include "ulib.h"

static uint8_t buf[16384];

int main(void) {
    uint8_t a[RANDOM_MAX], b[RANDOM_MAX];
    check(sys_random(a, sizeof a) == RANDOM_MAX && sys_random(b, sizeof b) == RANDOM_MAX, "256 bytes, twice");
    check(memcmp(a, b, sizeof a) != 0, "and they differ");
    check(sys_random(a, 0) == 0, "no bytes is fine");
    check(sys_random(a, RANDOM_MAX + 1) == -EINVAL, "more than 256 at once is refused");
    check(sys_random((void*)0x10, 16) == -EFAULT, "a bad pointer is refused");
    check(sys_random((void*)0xC0000000u, 16) == -EFAULT, "so is a kernel address");
    uint8_t guard[40];
    memset(guard, 0x5A, sizeof guard);
    sys_random(guard + 8, 24);
    int kept = 1;
    for (int i = 0; i < 8; i++) kept &= guard[i] == 0x5A && guard[32 + i] == 0x5A;
    check(kept, "only the bytes asked for are written");

    for (uint32_t i = 0; i < sizeof buf; i += RANDOM_MAX) sys_random(buf + i, RANDOM_MAX);
    uint32_t ones = 0, counts[256] = { 0 };
    for (uint32_t i = 0; i < sizeof buf; i++) {
        counts[buf[i]]++;
        for (int bit = 0; bit < 8; bit++) ones += (buf[i] >> bit) & 1;
    }
    // 131072 bits: the count of ones should be 65536 give or take 181
    // (one standard deviation); 1000 is more than five of them.
    check(ones > 64536 && ones < 66536, "about half the bits are ones");
    // Each byte value is expected 64 times, give or take 8.
    int spread = 1;
    for (int v = 0; v < 256; v++) spread &= counts[v] > 16 && counts[v] < 128;
    check(spread, "every byte value turns up about as often as the rest");
    // No 8-byte run turns up twice at a 256-byte boundary (a stuck or cycling generator would do this).
    int fresh = 1;
    for (uint32_t i = 0; i < sizeof buf; i += RANDOM_MAX)
        for (uint32_t j = i + RANDOM_MAX; j < sizeof buf; j += RANDOM_MAX) fresh &= memcmp(buf + i, buf + j, 8) != 0;
    check(fresh, "no call repeats another");
    return check_summary("rndtest");
}

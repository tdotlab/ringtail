// The user stack starts as one page and grows on demand, up to 1 MB.
// "stack" recurses 512 KB deep and checks every frame on the way
// back. "stack overflow" recurses forever: the kernel must kill the
// program when it runs past the limit, and nothing else.
#include "ulib.h"

static int forever;

static uint32_t dive(uint32_t depth, uint32_t max) {
    volatile uint8_t frame[1024];
    for (int i = 0; i < 1024; i += 64) frame[i] = (uint8_t)(depth + i);
    uint32_t sum = 0;
    if (forever || depth < max) sum = dive(depth + 1, max);
    for (int i = 0; i < 1024; i += 64)
        if (frame[i] != (uint8_t)(depth + i)) return 0xFFFFFFFF;
    return sum == 0xFFFFFFFF ? sum : sum + 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "overflow") == 0) {
        puts("stack: recursing forever");
        forever = 1;
        dive(0, 0);
        puts("stack: survived infinite recursion?");
        return 1;
    }
    uint32_t frames = dive(0, 499);
    check(frames == 500, "500 frames of 1 KB each, all intact");
    return check_summary("stack");
}

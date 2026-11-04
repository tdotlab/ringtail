// Count to a number (default 4), one step a second. Useful for
// trying Ctrl+C and background jobs: run count 30 &
#include "ulib.h"

int main(int argc, char** argv) {
    uint32_t n = 4;
    if (argc > 1 && parse_uint(argv[1], &n)) {
        puts("usage: count [n]");
        return 1;
    }
    for (uint32_t i = 0; i <= n; i++) {
        printf("count: %u\n", i);
        if (i < n) sys_sleep(1);
    }
    puts("count done");
    return 0;
}

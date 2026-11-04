// Read through a null pointer. Page 0 is never mapped, so this page
// faults and the kernel kills just this program.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    volatile uint32_t x = *(volatile uint32_t*)0;
    (void)x;
    puts("fault did not happen");
    return 1;
}

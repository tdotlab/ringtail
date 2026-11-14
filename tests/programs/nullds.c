// Load null data segments, then make a system call. On 32-bit x86 a
// kernel that trusted the caller's segment registers would fault on
// its first memory access. In 64-bit mode the CPU ignores ds and es
// for addressing, so the test checks that a call made with null ones
// comes back, with its result.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    static const char msg[] = "nullds: system call with null ds/es worked\n";
    int r;
    __asm__ volatile(
        "mov %%ds, %%r8d\n mov %%es, %%r9d\n"
        "xor %%eax, %%eax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
        "mov %1, %%eax\n int $0x80\n"
        "mov %%r8w, %%ds\n mov %%r9w, %%es\n"
        : "=a"(r)
        : "i"(SYS_PRINT), "D"(msg), "S"(sizeof msg - 1)
        : "r8", "r9", "memory");
    return r == (int)sizeof msg - 1 ? 0 : 1;
}

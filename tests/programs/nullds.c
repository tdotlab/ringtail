// Load null data segments, then make a system call. A kernel that
// trusted the caller's segment registers would fault on its first
// memory access; RingTail's entry stub loads its own. In 64-bit mode
// the CPU ignores ds and es for addressing, so there the test checks
// only that a call made with null ones comes back.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    static const char msg[] = "nullds: system call with null ds/es worked\n";
    int r;
#ifdef __x86_64__
    __asm__ volatile(
        "mov %%ds, %%r8d\n mov %%es, %%r9d\n"
        "xor %%eax, %%eax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
        "mov %1, %%eax\n int $0x80\n"
        "mov %%r8w, %%ds\n mov %%r9w, %%es\n"
        : "=a"(r)
        : "i"(SYS_PRINT), "D"(msg), "S"(sizeof msg - 1)
        : "r8", "r9", "memory");
#else
    __asm__ volatile(
        "push %%ds\n push %%es\n"
        "xor %%eax, %%eax\n mov %%ax, %%ds\n mov %%ax, %%es\n"
        "mov %1, %%eax\n int $0x80\n"
        "pop %%es\n pop %%ds\n"
        : "=a"(r)
        : "i"(SYS_PRINT), "b"(msg), "c"(sizeof msg - 1)
        : "memory");
    // The kernel returns with the caller's selectors, so restore
    // usable ones before touching memory again (done by the pops).
#endif
    return r == (int)sizeof msg - 1 ? 0 : 1;
}

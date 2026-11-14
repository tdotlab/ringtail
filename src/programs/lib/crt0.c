// Program entry. The loader lays out the stack as if _start(argc,
// argv) had just been called: a zero return address, then argc and
// argv. On i386 arguments are passed on the stack, so _start is an
// ordinary C function. On x86_64 they are passed in registers, so a
// few instructions move them there first.
#include "ulib.h"

int main(int argc, char** argv);

#ifdef __x86_64__
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov 8(%rsp), %edi\n"    // argc
        "    mov 16(%rsp), %rsi\n"   // argv
        "    jmp _start_c\n");        // with the zero return address still on the stack
#define START _start_c
void START(int argc, char** argv) __attribute__((used, noreturn));
#else
#define START _start
#endif

void START(int argc, char** argv) {
    sys_exit(main(argc, argv));
}

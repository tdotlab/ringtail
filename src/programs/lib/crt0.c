// Program entry. The loader lays out the stack as if _start(argc,
// argv) had just been called: a zero return address, then argc and
// argv. The C calling convention passes arguments in registers, so a
// few instructions move them there before the C part runs.
#include "ulib.h"

int main(int argc, char** argv);

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov 8(%rsp), %edi\n"    // argc
        "    mov 16(%rsp), %rsi\n"   // argv
        "    jmp _start_c\n");        // with the zero return address still on the stack

void _start_c(int argc, char** argv) __attribute__((used, noreturn));

void _start_c(int argc, char** argv) {
    sys_exit(main(argc, argv));
}

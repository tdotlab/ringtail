// crash: the general-protection proof, and the v0.6.0 crash
// program brought back as a disk file. It executes cli, a
// privileged instruction, in ring 3, which faults with a general
// protection exception (vector 13). In v0.6.0 that halted the
// whole OS with a bare exception number. In v0.7.1 it kills
// only this program: the exception handler prints the vector,
// sees a user task, and exits it, and the shell keeps running.
// Same packaging as the other programs: separate binary, linked
// at 0x08000000, entry at offset 0, and it never reaches its
// exit call, because the fault happens first.
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    __asm__ volatile("cli");
    const char* msg = "crash did not fault\n";
    sys_print(msg, prog_strlen(msg));
    sys_exit();
}

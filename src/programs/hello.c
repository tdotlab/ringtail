// hello: the smallest real program. It prints one line and
// exits. It is NOT part of the kernel image anymore: it is
// compiled separately, linked at 0x08000000, packed onto the disk
// image by tools/mkdisk.py, and loaded by the kernel's loader
// when the shell runs "run hello". _start must be the first
// byte of the binary (see programs.ld), because the loader has
// no header to read an entry offset from: offset 0 IS the entry.
// The program never returns: it ends in sys_exit, which the
// kernel implements as task_exit, and user_entry has nowhere to
// return to if a program did return.
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    const char* msg = "Hello from disk!\n";
    sys_print(msg, prog_strlen(msg));
    sys_exit();
}

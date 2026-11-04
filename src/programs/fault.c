// fault: the paging proof, and the replacement for v0.6.0's
// crash program (which faulted by executing a privileged
// instruction, cli, in ring 3). This one faults by memory: it
// reads address 0, which is a present page, but a supervisor
// page (the kernel's identity map), so a ring 3 read of it is a
// page fault. The handler prints the faulting address (0) and
// the error code (5: present page, read, user mode) and halts.
// That halt is the honest limit of this slice: paging gives the
// OS the fault address and the ability to kill the program, but
// per-task fault recovery (kill the program, keep the OS
// running) is not built yet, so a faulting program still takes
// the whole OS down, just with a much better diagnostic than
// v0.6.0's bare exception number.
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    volatile unsigned int x = *(volatile unsigned int*)0;
    (void)x;
    const char* msg = "fault did not happen\n";
    sys_print(msg, prog_strlen(msg));
    sys_exit();
}

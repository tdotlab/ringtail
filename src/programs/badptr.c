// badptr: the syscall fault proof. It asks the kernel to print
// from an unmapped address (0x09000000: no page table entry in
// this task's directory), which passes the syscall's shallow
// validation (the pointer is non-null and the length is small)
// and then faults when the kernel actually reads the bytes, in
// ring 0, during the print syscall. The fault is still the user
// program's fault, because it supplied the bad pointer, so
// v0.7.1 kills only this program, and the shell keeps running.
// If fault recovery had been decided by privilege level instead
// of by task, this case would wrongly halt the OS: the fault
// happened in kernel mode, but the blame belongs to the caller.
// Same packaging as the other programs: separate binary, linked
// at 0x08000000, entry at offset 0.
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    sys_print((const char*)0x09000000u, 10);
    const char* msg = "badptr did not fault\n";
    sys_print(msg, prog_strlen(msg));
    sys_exit();
}

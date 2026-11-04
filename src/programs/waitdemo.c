// waitdemo: the WAIT syscall proof. It prints that it is
// waiting, reads the current mouse state, then waits with
// that state: WAIT returns at once if a key is already
// queued or the mouse already moved, otherwise it sleeps
// until a key or mouse interrupt wakes it. Then it prints
// that it woke up, and exits. The key that wakes it is not
// consumed by WAIT: it stays queued, so in text mode the
// shell will also see it as typed input after this program
// exits, which is expected, not a bug. Run it, wait a few
// seconds with no input (nothing more prints, proving it
// blocks rather than sleeping on a timer), then press a
// key or move the mouse: "Woke up on input." appears, and
// the program exits. Same packaging as the other programs:
// separate binary, linked at 0x08000000, entry at offset 0.
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    const char* msg = "Waiting for input...\n";
    sys_print(msg, prog_strlen(msg));
    mouse_state_t ms;
    sys_mouse_state(&ms);
    sys_wait(ms.x, ms.y, ms.buttons);
    const char* woke = "Woke up on input.\n";
    sys_print(woke, prog_strlen(woke));
    sys_exit();
}

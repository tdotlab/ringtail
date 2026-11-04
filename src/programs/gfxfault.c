// gfxfault: the fault-log proof. It enters graphics mode,
// then faults exactly like badptr does: it asks the kernel
// to print from an unmapped address (0x09000000), which
// faults in ring 0 during the print syscall, and v0.7.1
// kills only this program. The difference from badptr is
// the mode: the fault happens while graphics owns the
// display, so the diagnostic printed to the text screen
// is invisible at the time, and the text restore on
// cleanup (from the copy saved at GFX_BEGIN, before the
// fault) overwrites it, so the shell never shows it. The
// fault log, kept in RAM, survives that restore, so after
// this program dies and the prompt returns, `faults` still
// shows the PAGE FAULT entry, with its task id and tick.
// If it somehow does not fault, it ends graphics cleanly
// and exits in text mode, rather than leaving the OS in
// graphics mode with no program drawing. Same packaging
// as the other programs: separate binary, linked at
// 0x08000000, entry at offset 0. Name is exactly 8
// characters, to fit 8.3 ("gfxfault", no extension).
#include "syscall_stub.h"

__attribute__((section(".text._start"))) void _start(void) {
    if (sys_gfx_begin() != 0) {
        const char* msg = "Graphics busy: another program owns it\n";
        sys_print(msg, prog_strlen(msg));
        sys_exit();
    }
    sys_print((const char*)0x09000000u, 10);
    // Unreached if the fault worked: end graphics first,
    // so a non-faulting run still exits cleanly in text.
    sys_gfx_end();
    const char* msg = "gfxfault did not fault\n";
    sys_print(msg, prog_strlen(msg));
    sys_exit();
}

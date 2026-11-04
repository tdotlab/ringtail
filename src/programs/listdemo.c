// listdemo: the LIST and SPAWN demo. It lists every file on
// disk by calling LIST with increasing index until it fails,
// then spawns hello (which should succeed) and a missing file
// (which should fail), printing the result of each step, so a
// text-mode run proves both syscalls without the GUI. Two
// failure-path checks come first, and both are safe to run:
// a too-small buffer size is passed with a large buffer, so
// even a kernel that ignored the size and copied anyway could
// not overflow the stack, it would only return the wrong
// value, which the check catches. A null buffer is rejected
// by the kernel before any copy, so it cannot fault.
// Programs are freestanding, so this carries its own tiny
// printing helpers, like the other demo programs.
#include "syscall_stub.h"

static void print_s(const char* s) {
    sys_print(s, prog_strlen(s));
}
static void print_num(int n) {
    char tmp[12];
    int i = 0;
    if (n == 0) { print_s("0"); return; }
    while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) { char c = tmp[--i]; sys_print(&c, 1); }
}

__attribute__((section(".text._start"))) void _start(void) {
    // Too-small size, with a roomy buffer: if the kernel
    // checked the size, this fails; if it did not, it would
    // copy at most a 12-character name into a 64-byte buffer,
    // so the program survives and the wrong return is caught.
    char bigbuf[64];
    if (sys_list(0, bigbuf, 2) < 0) print_s("small buffer correctly failed\n");
    else print_s("ERROR: small buffer succeeded\n");
    // Null buffer: rejected before any copy, so no fault.
    if (sys_list(0, (char*)0, 16) < 0) print_s("null buffer correctly failed\n");
    else print_s("ERROR: null buffer succeeded\n");
    // The real list, index 0 upward until LIST fails.
    char name[16];
    int count = 0;
    while (1) {
        if (sys_list(count, name, 16) != 0) break;
        print_num(count);
        print_s(": ");
        print_s(name);
        print_s("\n");
        count++;
    }
    print_s("listed ");
    print_num(count);
    print_s(" files\n");
    int id = sys_spawn("hello");
    if (id >= 0) {
        print_s("spawned hello as task ");
        print_num(id);
        print_s("\n");
    } else {
        print_s("ERROR: spawn hello failed\n");
    }
    int bad = sys_spawn("missing");
    if (bad < 0) print_s("spawn missing correctly failed\n");
    else print_s("ERROR: spawn missing succeeded\n");
    sys_exit();
}

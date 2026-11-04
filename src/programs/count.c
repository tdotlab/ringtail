// count: prints count: 0 through count: 4, one per second, then
// "count done", then exits. It proves a loaded program can keep
// state, format numbers, and sleep, all through syscalls, while
// the shell stays responsive and the kernel counter (if any)
// keeps counting alongside it. Formatting is program work: the
// kernel's print syscall only prints bytes it is given, so the
// number-to-text conversion lives here, in the program, not in
// the kernel. Same packaging as hello: separate binary, linked
// at 0x08000000, entry at offset 0, ends in sys_exit.
#include "syscall_stub.h"

static void utoa(int n, char* out) {
    char tmp[12];
    int i = 0;
    if (n == 0) { out[0] = '0'; out[1] = 0; return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

__attribute__((section(".text._start"))) void _start(void) {
    for (int i = 0; i <= 4; i++) {
        char line[32];
        const char* prefix = "count: ";
        int p = 0;
        while (prefix[p]) { line[p] = prefix[p]; p++; }
        char num[12];
        utoa(i, num);
        int q = 0;
        while (num[q]) { line[p++] = num[q++]; }
        line[p++] = '\n';
        line[p] = 0;
        sys_print(line, p);
        sys_sleep(1);
    }
    const char* done = "count done\n";
    sys_print(done, prog_strlen(done));
    sys_exit();
}

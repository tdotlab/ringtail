// Run a privileged instruction in ring 3. The CPU refuses with a
// general protection fault and the kernel kills just this program.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    __asm__ volatile("cli");
    puts("crash did not fault");
    return 1;
}

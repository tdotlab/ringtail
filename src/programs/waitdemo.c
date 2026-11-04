// Sleep in the kernel until input arrives (a key or mouse movement).
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    puts("Waiting for input...");
    mouse_state_t m;
    sys_mouse_state(&m);
    int r = sys_wait(m.x, m.y, m.buttons);
    if (r < 0) {
        printf("wait: %s\n", strerror(r));
        return 1;
    }
    puts("Woke up on input.");
    return 0;
}

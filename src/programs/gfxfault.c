// Fault while owning the display. The kernel must give the screen
// back to text mode when it kills the program, or the machine would
// be stuck in graphics with nobody drawing.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = sys_gfx_begin();
    if (r < 0) {
        printf("gfxfault: cannot take the display: %s\n", strerror(r));
        return 1;
    }
    sys_gfx_clear(4);
    volatile uint32_t x = *(volatile uint32_t*)0;
    (void)x;
    sys_gfx_end();
    puts("gfxfault did not fault");
    return 1;
}

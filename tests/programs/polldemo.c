// SYS_POLL. Prints how a poll with nothing waiting ends and how long it
// took, then waits for a key in a long poll (the test types one).
// With an argument it polls for an hour instead, for the test to kill.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argv;
    sys_tty_mode(TTY_RAW | TTY_NOSIG);
    if (argc > 1) {
        printf("polling\n");
        int r = sys_poll(3600 * 100);
        printf("poll ended: %d\n", r);
        return 0;
    }
    uint32_t t0 = sys_ticks();
    int r = sys_poll(30);
    printf("timeout: %d after %u ticks\n", r, sys_ticks() - t0);
    printf("zero: %d\n", sys_poll(0));
    printf("ready\n");
    r = sys_poll(1000);
    unsigned char c = 0;
    if (r == 1) sys_read(STDIN_FILENO, &c, 1);
    printf("input: %d key %02x\n", r, c);
    printf("again: %d\n", sys_poll(10));
    sys_tty_mode(0);
    return 0;
}

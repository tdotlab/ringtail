// Print the bytes raw mode delivers, in hex, one read per line.
// With an argument, Ctrl+C arrives as a byte too (TTY_NOSIG).
// q quits.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argv;
    int old = sys_tty_mode(TTY_RAW | (argc > 1 ? TTY_NOSIG : 0));
    if (old < 0) { printf("tty mode: %s\n", strerror(old)); return 1; }
    printf("raw mode on\n");
    for (;;) {
        unsigned char buf[32];
        int n = sys_read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) break;
        char line[3 * sizeof buf + 2];
        int k = 0;
        for (int i = 0; i < n; i++) k += snprintf(line + k, sizeof line - k, "%s%02x", i ? " " : "", buf[i]);
        printf("keys: %s\n", line);
        if (buf[0] == 'q') break;
    }
    sys_tty_mode(0);
    return 0;
}

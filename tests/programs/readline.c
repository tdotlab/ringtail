// Read lines from the terminal until end of file (Ctrl+D on an empty
// line), echoing each one back. Shows that stdin is a real file
// descriptor with cooked line editing.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char buf[128];
    int lines = 0;
    puts("Type lines; Ctrl+D on an empty line ends.");
    for (;;) {
        print("> ");
        int n = sys_read(STDIN_FILENO, buf, sizeof buf - 1);
        if (n < 0) {
            printf("readline: %s\n", strerror(n));
            return 1;
        }
        if (n == 0) break;
        buf[n] = 0;
        if (buf[n - 1] == '\n') buf[n - 1] = 0;
        printf("got \"%s\" (%d bytes)\n", buf, n);
        lines++;
    }
    printf("readline: %d lines\n", lines);
    return 0;
}

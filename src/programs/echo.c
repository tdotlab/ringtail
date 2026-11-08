// echo [-n] [TEXT...]    write the words, with one space between them
//
// -n leaves off the newline. To keep the blanks inside a piece of text
// as typed, quote it: echo "two   spaces".
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, newline = 1;
    if (a < argc && !strcmp(argv[a], "-n")) {
        newline = 0;
        a++;
    }
    char buf[1024];
    uint32_t n = 0;
    for (int i = a; i < argc; i++) {
        uint32_t len = (uint32_t)strlen(argv[i]);
        if (n + len + 2 > sizeof buf) {  // flush rather than truncate
            sys_write(STDOUT_FILENO, buf, n);
            n = 0;
        }
        if (len > sizeof buf - 2) {
            sys_write(STDOUT_FILENO, argv[i], len);
        } else {
            memcpy(buf + n, argv[i], len);
            n += len;
        }
        if (i + 1 < argc) buf[n++] = ' ';
    }
    if (newline) buf[n++] = '\n';
    if (n) sys_write(STDOUT_FILENO, buf, n);
    return 0;
}

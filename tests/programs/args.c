// Print the command line arguments, one per line.
#include "ulib.h"

int main(int argc, char** argv) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
    return argv[argc] == 0 ? 0 : 1;  // the array must be NULL terminated
}

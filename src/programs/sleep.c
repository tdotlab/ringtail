// sleep N: wait N seconds (at most an hour), then exit.
#include "ulib.h"

int main(int argc, char** argv) {
    uint32_t n;
    if (argc != 2 || parse_uint(argv[1], &n) != 0 || n > 3600) {
        dprintf(STDERR_FILENO, "usage: sleep SECONDS (0 to 3600)\n");
        return 1;
    }
    sys_sleep(n);
    return 0;
}

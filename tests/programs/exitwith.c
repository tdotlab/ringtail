// Exit with the status given as the first argument (0 if none).
// The tests use it to see how xargs treats a child's status.
#include "ulib.h"

int main(int argc, char** argv) {
    uint32_t v = 0;
    if (argc > 1 && parse_uint(argv[1], &v)) return 2;
    return (int)v;
}

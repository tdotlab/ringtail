// Program entry. The loader lays out the stack as if _start(argc,
// argv) had been called, so it is an ordinary C function.
#include "ulib.h"

int main(int argc, char** argv);

void _start(int argc, char** argv) {
    sys_exit(main(argc, argv));
}

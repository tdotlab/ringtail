// Draw with ANSI escapes so the test suite can check the screen.
#include "ulib.h"

int main(void) {
    print("\x1b[2J\x1b[H");             // clear, home
    print("\x1b[5;10HAT-5-10");          // row 5, column 10
    print("\x1b[31;44mRED\x1b[0m");      // red on blue, right after
    print("\x1b[7;1Habcdef\x1b[3D\x1b[KX");  // erase from d to the end, then X
    print("\x1b[8;1H\x1b[sSAVE\x1b[u!");  // save, write, restore, overwrite S
    print("\x1b[10;1H");
    return 0;
}

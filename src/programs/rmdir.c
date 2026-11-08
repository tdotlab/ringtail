// rmdir DIR...    remove empty directories
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (a == argc || (!dd && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: rmdir DIR...\n");
        return 2;
    }
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int r = sys_rmdir(argv[i]);
        if (r == -EBUSY) dprintf(STDERR_FILENO, "rmdir: %s: in use (someone's current directory)\n", argv[i]);
        else if (r < 0) fs_error("rmdir", argv[i], r);
        bad |= r < 0;
    }
    return bad;
}

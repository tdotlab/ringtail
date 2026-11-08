// rmdir DIR...    remove empty directories
#include "ulib.h"

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "", got) || argc < 2) {
        dprintf(STDERR_FILENO, "usage: rmdir DIR...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int r = sys_rmdir(argv[i]);
        if (r == -EBUSY) dprintf(STDERR_FILENO, "rmdir: %s: in use (someone's current directory)\n", argv[i]);
        else if (r < 0) fs_error("rmdir", argv[i], r);
        bad |= r < 0;
    }
    return bad;
}

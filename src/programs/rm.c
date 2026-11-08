// rm FILE...    delete files
//
// Files only: a directory is refused (rmdir takes an empty one).
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (a == argc || (!dd && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: rm FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int r = sys_delete(argv[i]);
        if (r == -EISDIR) dprintf(STDERR_FILENO, "rm: %s: is a directory (rmdir removes an empty one)\n", argv[i]);
        else if (r < 0) fs_error("rm", argv[i], r);
        bad |= r < 0;
    }
    return bad;
}

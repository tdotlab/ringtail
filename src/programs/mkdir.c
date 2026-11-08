// mkdir DIR...    make directories
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (a == argc || (!dd && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: mkdir DIR...\n");
        return 2;
    }
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int r = sys_mkdir(argv[i]);
        if (r < 0) {
            fs_error("mkdir", argv[i], r);
            bad = 1;
        }
    }
    return bad;
}

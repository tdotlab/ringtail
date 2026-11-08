// touch FILE...    create empty files
//
// A FILE that already exists is an error: RingTail's touch makes new
// files, and has no way to set a time on an old one.
#include "ulib.h"

int main(int argc, char** argv) {
    int a = 1, dd = 0;
    if (a < argc && !strcmp(argv[a], "--")) a++, dd = 1;
    if (a == argc || (!dd && argv[a][0] == '-' && argv[a][1])) {
        dprintf(STDERR_FILENO, "usage: touch FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = a; i < argc; i++) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT | O_EXCL);
        if (fd < 0) {
            fs_error("touch", argv[i], fd);
            bad = 1;
        } else {
            sys_close(fd);
        }
    }
    return bad;
}

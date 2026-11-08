// touch [-c] FILE...    make sure files exist
//
// A FILE that is not there is created empty. One that is there is left
// alone and that is not an error. RingTail has no call to set a time on
// an old file, so unlike the usual touch this does not bump its date.
// -c creates nothing: a missing FILE is skipped without a word.
#include "ulib.h"

int main(int argc, char** argv) {
    char got[32];
    if (getflags(&argc, argv, "c", got) || argc < 2) {
        dprintf(STDERR_FILENO, "usage: touch [-c] FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        dirent_t d;
        int r = sys_stat(argv[i], &d);
        if (r == 0) continue;  // there already: nothing to do
        if (r != -ENOENT) {
            fs_error("touch", argv[i], r);
            bad = 1;
        } else if (!has_flag(got, 'c')) {
            int fd = sys_open(argv[i], O_WRONLY | O_CREAT | O_EXCL);
            if (fd < 0) {
                fs_error("touch", argv[i], fd);
                bad = 1;
            } else {
                sys_close(fd);
            }
        }
    }
    return bad;
}

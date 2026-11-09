// dirname NAME...
//
// Prints NAME without its last component: "/usr/bin/sort" is "/usr/bin",
// "sort" is ".", "/usr/" is "/", "a//b" is "a". Trailing slashes are
// ignored.
#include "ulib.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "dirname: missing operand\nusage: dirname NAME...\n");
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        const char* s = argv[a];
        uint32_t end = (uint32_t)strlen(s);
        while (end > 1 && s[end - 1] == '/') end--;          // trailing slashes
        while (end > 0 && s[end - 1] != '/') end--;          // the last name
        while (end > 1 && s[end - 1] == '/') end--;          // the slashes before it
        if (end == 0) print(".\n");
        else {
            sys_write(STDOUT_FILENO, s, end);
            print("\n");
        }
    }
    return 0;
}

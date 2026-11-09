// basename NAME [SUFFIX]
// basename [-a] [-s SUFFIX] NAME...
//
// Prints NAME without its directories (and, if it is not the whole
// name, without a trailing SUFFIX). Trailing slashes are ignored; "/"
// is "/". With -a or -s, every operand is a name (-s names the suffix).
#include "ulib.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: basename NAME [SUFFIX]\n       basename [-a] [-s SUFFIX] NAME...\n");
    return 2;
}

static void base(const char* name, const char* suffix) {
    uint32_t end = (uint32_t)strlen(name);
    while (end > 1 && name[end - 1] == '/') end--;
    if (end == 1 && name[0] == '/') {  // nothing but slashes
        print("/\n");
        return;
    }
    uint32_t start = end;
    while (start > 0 && name[start - 1] != '/') start--;
    uint32_t len = end - start;
    if (suffix) {
        uint32_t sl = (uint32_t)strlen(suffix);
        if (sl && sl < len && !memcmp(name + start + len - sl, suffix, sl)) len -= sl;
    }
    sys_write(STDOUT_FILENO, name + start, len);
    print("\n");
}

int main(int argc, char** argv) {
    const char* suffix = 0;
    int multiple = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char* o = argv[a] + 1;
        if (!strcmp(argv[a], "--")) {
            a++;
            break;
        }
        for (; *o; o++) {
            if (*o == 'a') multiple = 1;
            else if (*o == 's') {
                const char* v = o + 1;
                if (!*v) {
                    if (++a == argc) return usage();
                    v = argv[a];
                }
                suffix = v;
                multiple = 1;
                break;
            } else {
                dprintf(STDERR_FILENO, "basename: invalid option -- '%c'\n", *o);
                return usage();
            }
        }
    }
    int n = argc - a;
    if (n < 1) {
        dprintf(STDERR_FILENO, "basename: missing operand\n");
        return usage();
    }
    if (!multiple) {
        if (n > 2) {
            dprintf(STDERR_FILENO, "basename: extra operand '%s'\n", argv[a + 2]);
            return usage();
        }
        base(argv[a], n == 2 ? argv[a + 1] : 0);
        return 0;
    }
    for (; a < argc; a++) base(argv[a], suffix);
    return 0;
}

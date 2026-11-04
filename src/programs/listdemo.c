// List the disk through LIST, then spawn a program and wait for it.
#include "ulib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char name[16];
    check(sys_list(0, name, 2) == -EINVAL, "a buffer too small gives EINVAL");
    check(sys_list(0, 0, 16) == -EFAULT, "a null buffer gives EFAULT");
    int count = 0;
    while (sys_list(count, name, sizeof name) == 0) {
        printf("  %d: %s\n", count, name);
        count++;
    }
    check(count > 0, "listed some files");
    check(sys_list(count, name, sizeof name) == -ENOENT, "past the end gives ENOENT");
    int id = sys_spawn("HELLO");
    check(id > 0, "spawn HELLO");
    check(sys_join(id) == 0, "HELLO exited with status 0");
    return check_summary("listdemo");
}

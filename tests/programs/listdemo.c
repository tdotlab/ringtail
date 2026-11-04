// List the disk through LIST, then spawn programs and wait for them.
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
    const char* argv2[] = { "ARGS", "one", "two words", 0 };
    id = sys_spawn_args("ARGS", argv2);
    check(id > 0 && sys_join(id) == 0, "spawn ARGS with arguments");
    const char* bad[] = { "ARGS", (const char*)0x10, 0 };
    check(sys_spawn_args("ARGS", bad) == -EFAULT, "a bad argument pointer gives EFAULT");
    check(sys_spawn_args("ARGS", (const char* const*)0xC0000000) == -EFAULT, "a kernel argv gives EFAULT");
    return check_summary("listdemo");
}

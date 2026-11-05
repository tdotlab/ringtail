// SPAWN_FOREGROUND: the child owns the terminal from its first
// instruction, not from whenever its parent gets round to SYS_TTY_FG.
// Before it, a shell that ran before its creator handed it the
// terminal read as a background job and ended at once.
#include "ulib.h"

static int spawn_as(const char* role, int flags) {
    const char* argv[] = { "fgspawn", role, 0 };
    spawn_opts_t o = { flags, 0, { -1, -1, -1 } };
    return sys_spawn_opts("fgspawn", argv, &o);
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "first")) {
        // The very first thing: foreground only, so -EPERM if not.
        sys_exit(sys_tty_mode(0) >= 0 ? 0 : 1);
    }
    if (argc == 2 && !strcmp(argv[1], "bg"))
        sys_exit(spawn_as("first", SPAWN_NEW_JOB | SPAWN_FOREGROUND) == -EPERM ? 0 : 1);

    int good = 0;
    for (int i = 0; i < 20; i++) {
        int id = spawn_as("first", SPAWN_NEW_JOB | SPAWN_FOREGROUND);
        if (id < 0) break;
        int status = sys_join(id);
        sys_wait_job(id);
        if (status == 0 && sys_tty_fg(0) == 0) good++;
    }
    check(good == 20, "a foreground child has the terminal before it runs, 20 times out of 20");
    check(sys_tty_mode(0) >= 0, "and the parent gets it back each time");

    int id = spawn_as("bg", SPAWN_NEW_JOB);
    check(id > 0 && sys_join(id) == 0, "a background job cannot spawn into the foreground");
    sys_wait_job(id);
    check(sys_tty_mode(0) >= 0, "and the terminal stayed with the parent");

    check(spawn_as("first", 8) == -EINVAL, "an unknown spawn flag is refused");
    check(spawn_as("first", SPAWN_NEW_JOB | SPAWN_JOIN | SPAWN_FOREGROUND) == -EINVAL,
          "a new job and joining one at once is still refused");
    return check_summary("fgspawn");
}

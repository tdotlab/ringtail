// kill(-job, sig): a negative id signals every task of a job, so a
// command started as a job can be stopped together with whatever it
// started (barb stops a command that way). Killing the leader alone
// leaves its children running, which is the case this exists for.
#include "ulib.h"

// The job leader: starts a child in its own job, tells the parent the
// child's id through the pipe on fd 1, then waits for ever.
static int leader(void) {
    const char* argv[] = { "killjob", "sleeper", 0 };
    int child = sys_spawn_args("killjob", argv);
    char msg[16];
    int n = snprintf(msg, sizeof msg, "%d\n", child);
    sys_write(STDOUT_FILENO, msg, (uint32_t)n);
    for (;;) sys_sleep(100);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "leader")) return leader();
    if (argc == 2 && !strcmp(argv[1], "sleeper")) for (;;) sys_sleep(100);

    int fds[2];
    check(sys_pipe(fds) == 0, "a pipe for the leader to report on");
    const char* argv2[] = { "killjob", "leader", 0 };
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { -1, fds[1], -1 }, -1 };
    int job = sys_spawn_opts("killjob", argv2, &o);
    sys_close(fds[1]);
    check(job > 0, "the leader starts as a job of its own");
    char buf[16] = { 0 };
    int n = 0;
    for (int r; n < (int)sizeof buf - 1 && (r = sys_read(fds[0], buf + n, 1)) > 0 && buf[n] != '\n';) n++;
    buf[n] = 0;
    uint32_t child = 0;
    check(n > 0 && !parse_uint(buf, &child) && child > 0, "the leader reports its child");
    sys_close(fds[0]);

    check(sys_kill(-job, 0) == 0, "signal 0 finds the job alive");
    check(sys_kill((int)child, 0) == 0, "and the child in it");
    check(sys_kill(-job, NSIG) == -EINVAL, "a signal that does not exist is refused");
    check(sys_kill(-job, -1) == -EINVAL, "and a negative one");
    check(sys_kill(-job, SIGTERM) == 0, "SIGTERM goes to the whole job");
    check(sys_join(job) == EXIT_SIGNAL(SIGTERM), "the leader ends by it");
    // Not join(child) at once: if the signal missed it, that would wait for ever.
    uint32_t until = sys_ticks() + 200;
    while (sys_kill((int)child, 0) == 0 && sys_ticks() < until) sys_pollfd(0, 0, 10);   // 10 ms naps
    int alive = sys_kill((int)child, 0) == 0;
    if (alive) sys_kill((int)child, SIGKILL);
    check(!alive && sys_join((int)child) == EXIT_SIGNAL(SIGTERM), "and so does the child, which killing the leader alone would miss");
    sys_wait_job(job);
    check(sys_kill(-job, 0) == -ESRCH, "after that the job has nobody alive");
    check(sys_kill(-job, SIGTERM) == -ESRCH, "and signalling it says so");
    check(sys_kill(-999999, SIGTERM) == -ESRCH, "a job that never was is not found");
    check(sys_kill((int)0x80000000u, SIGTERM) == -ESRCH, "nor is the most negative id");
    check(sys_kill(-sys_getpid(), 0) == 0, "a program can find its own job");
    return check_summary("killjob");
}

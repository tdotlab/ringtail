// Pipes and dup2: data in order, wrap-around, EOF, EPIPE, bad
// arguments, and pipes shared with child programs.
#include "ulib.h"

static char big[20000];

// Read until EOF into buf; returns the byte count, or -errno.
static int slurp(int fd, char* buf, int size) {
    int got = 0, n;
    while (got < size && (n = sys_read(fd, buf + got, (uint32_t)(size - got))) > 0) got += n;
    return n < 0 ? n : got;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int p[2], q[2];
    char buf[64];

    check(sys_pipe(p) == 0 && p[0] == 3 && p[1] == 4, "pipe gives the lowest free descriptors");
    check(sys_write(p[1], "hello", 5) == 5, "write into the pipe");
    check(sys_read(p[0], buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0, "read the same bytes back");
    check(sys_read(p[1], buf, 1) == -EBADF, "the write end cannot be read");
    check(sys_write(p[0], "x", 1) == -EBADF, "the read end cannot be written");
    check(sys_seek(p[0], 0, SEEK_SET) == -EINVAL, "pipes do not seek");

    // Move the ring's start, then fill it exactly so the data wraps.
    sys_write(p[1], big, 100);
    sys_read(p[0], big, 100);
    for (int i = 0; i < 4096; i++) big[i] = (char)(i * 13 + 7);
    check(sys_write(p[1], big, 4096) == 4096, "a full 4 KB fits without blocking");
    static char back[4096];
    int got = 0, n;
    while (got < 4096 && (n = sys_read(p[0], back + got, 1000)) > 0) got += n;
    check(got == 4096 && memcmp(back, big, 4096) == 0, "and comes out intact across the wrap");

    check(sys_dup2(p[1], 9) == 9, "dup2 copies the write end");
    check(sys_dup2(9, 9) == 9, "dup2 onto itself is a no-op");
    check(sys_dup2(5, 6) == -EBADF && sys_dup2(p[1], MAX_FDS) == -EBADF, "dup2 rejects bad descriptors");
    sys_close(p[1]);
    check(sys_write(9, "abc", 3) == 3, "the copy still writes");
    sys_close(9);
    check(sys_read(p[0], buf, sizeof buf) == 3, "buffered data outlives the writers");
    check(sys_read(p[0], buf, sizeof buf) == 0, "then end of file");
    sys_close(p[0]);

    sys_pipe(p);
    sys_close(p[0]);
    check(sys_write(p[1], "x", 1) == -EPIPE, "writing with no reader gives EPIPE");
    sys_close(p[1]);

    check(sys_pipe((int*)0) == -EFAULT && sys_pipe((int*)0xC0000000) == -EFAULT, "bad pointers give EFAULT");
    check(sys_pipe(p) == 0 && p[0] == 3, "and leak no descriptors");
    sys_close(p[0]);
    sys_close(p[1]);
    int opened = 0, r;
    while ((r = sys_pipe(p)) == 0) opened += 2;
    check(r == -EMFILE && opened == 12, "pipe stops at the descriptor limit");
    for (int fd = 3; fd < MAX_FDS; fd++) sys_close(fd);

    // A child's output, through a pipe.
    sys_pipe(p);
    const char* hello[] = { "hello", 0 };
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { -1, p[1], -1 } };
    int id = sys_spawn_opts("HELLO", hello, &o);
    sys_close(p[1]);
    got = slurp(p[0], buf, sizeof buf - 1);
    if (got >= 0) buf[got] = 0;
    check(id > 0 && got == 17 && strcmp(buf, "Hello from disk!\n") == 0, "a child writes into a pipe");
    check(sys_join(id) == 0, "and exits normally");
    sys_close(p[0]);

    // Through a filter and back: more than the pipe holds, so both
    // sides block in turn.
    sys_pipe(p);
    sys_pipe(q);
    const char* wc[] = { "wc", 0 };
    spawn_opts_t w = { SPAWN_NEW_JOB, 0, { p[0], q[1], -1 } };
    id = sys_spawn_opts("WC", wc, &w);
    sys_close(p[0]);
    sys_close(q[1]);
    memset(big, 'x', sizeof big);
    for (int i = 99; i < (int)sizeof big; i += 100) big[i] = '\n';
    check(sys_write(p[1], big, sizeof big) == (int)sizeof big, "20000 bytes into a filter");
    sys_close(p[1]);
    got = slurp(q[0], buf, sizeof buf - 1);
    if (got >= 0) buf[got] = 0;
    check(got > 0 && strcmp(buf, "    200     200   20000\n") == 0, "wc counts them all");
    sys_join(id);
    sys_close(q[0]);

    // A pipeline whose first program is gone before the second starts.
    sys_pipe(p);
    sys_pipe(q);
    spawn_opts_t first = { SPAWN_NEW_JOB, 0, { -1, p[1], -1 } };
    int lead = sys_spawn_opts("HELLO", hello, &first);
    sys_join(lead);
    sys_sleep(1);  // long enough for it to be reaped
    spawn_opts_t second = { SPAWN_JOIN, lead, { p[0], q[1], -1 } };
    int sec = sys_spawn_opts("WC", wc, &second);
    check(lead > 0 && sec > 0, "a job can still be joined after its leader ended");
    sys_close(p[0]);
    sys_close(p[1]);
    sys_close(q[1]);
    got = slurp(q[0], buf, sizeof buf - 1);
    if (got >= 0) buf[got] = 0;
    check(strcmp(buf, "      1       3      17\n") == 0, "and the data waited in the pipe");
    sys_close(q[0]);
    if (sec > 0) sys_join(sec);

    return check_summary("pipetest");
}

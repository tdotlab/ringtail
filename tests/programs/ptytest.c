// Pseudo-terminals: making one, a program on the slave reading lines
// typed into the master (echo, backspace, end of file), raw mode and
// its key sequences, Ctrl+C to the pty's foreground job, the size, what
// sys_print, sys_set_color and /dev/tty do on a pty, the foreground
// rules for spawning onto one, end of file at the master once every
// slave is closed, and the hangup when the master closes. The programs
// on the slaves are this program again, with a role.
#include "ulib.h"
#include "keymap.h"

static volatile int hups;
static void on_hup(int sig) { hups += sig == SIGHUP; }

static int child(const char* role) {
    char buf[128];
    if (!strcmp(role, "cat")) {  // each line back as got:<line>, until quit
        for (;;) {
            int n = sys_read(0, buf, sizeof buf - 1);
            if (n <= 0) return 10 - n;
            buf[n] = 0;
            if (!strcmp(buf, "quit\n")) return 0;
            dprintf(1, "got:%s", buf);
        }
    }
    if (!strcmp(role, "eof")) {  // the read after Ctrl+D
        return sys_read(0, buf, sizeof buf) == 0 ? 0 : 20;
    }
    if (!strcmp(role, "raw")) {  // in raw mode, the bytes of one key, in hex
        if (sys_tty_mode(TTY_RAW) < 0) return 30;
        dprintf(1, "ready\n");
        int n = sys_read(0, buf, sizeof buf);
        for (int i = 0; i < n; i++) dprintf(1, "%02x.", (uint8_t)buf[i]);
        dprintf(1, "|\n");
        return n > 0 ? 0 : 31;
    }
    if (!strcmp(role, "info")) {
        tty_size_t s = { 0, 0 };
        sys_tty_size(0, &s);
        dprintf(1, "size %dx%d\n", s.cols, s.rows);
        sys_print("printed\n", 8);
        sys_set_color(0x1E);
        int t = sys_open(DEV_TTY, O_WRONLY);
        if (t < 0 || sys_fdtype(t) != FDTYPE_TTY) return 40;
        sys_write(t, "devtty\n", 7);
        return 0;
    }
    if (!strcmp(role, "sleep")) {  // until a signal ends it
        dprintf(1, "ready\n");
        for (;;) sys_sleep(100);
    }
    if (!strcmp(role, "hup")) {  // catches SIGHUP; then reads end of file and writes fail
        signal(SIGHUP, on_hup);
        dprintf(1, "ready\n");
        int r1 = sys_read(0, buf, sizeof buf);  // the hangup ends this one, one way or the other
        int r2 = sys_read(0, buf, sizeof buf);
        int w = sys_write(1, "x", 1);
        return (hups == 1 ? 0 : 1) | (r1 == 0 || r1 == -EINTR ? 0 : 2) | (r2 == 0 ? 0 : 4) | (w == -EIO ? 0 : 8);
    }
    return 99;
}

// Starts this program as role on the slave, as its terminal.
static int start(int slave, const char* role, int flags) {
    const char* argv[] = { "ptytest", role, 0 };
    spawn_opts_t o = { flags, 0, { slave, slave, slave }, -1 };
    return sys_spawn_opts("ptytest", argv, &o);
}

static int start_fg(int slave, const char* role) { return start(slave, role, SPAWN_NEW_JOB | SPAWN_FOREGROUND); }

// What the master reads until want has appeared (or about two seconds
// pass), kept in got; how much there is.
static char got[2048];
static int got_len;

static const char* has(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncmp(hay, needle, n)) return hay;
    return 0;
}

static int expect(int m, const char* want) {
    got_len = 0;
    got[0] = 0;
    for (int tries = 0; tries < 40; tries++) {
        pollfd_t p = { m, POLLIN, 0 };
        if (sys_pollfd(&p, 1, 50) == 1 && (p.revents & POLLIN)) {
            int n = sys_read(m, got + got_len, sizeof got - 1 - got_len);
            if (n <= 0) break;
            got_len += n;
            got[got_len] = 0;
        }
        if (has(got, want)) return 1;
    }
    return 0;
}

static int events(int fd, int want) {
    pollfd_t p = { fd, want, 0 };
    return sys_pollfd(&p, 1, 0) < 0 ? -1 : p.revents;
}

static int put(int m, const char* s) { return sys_write(m, s, strlen(s)); }

int main(int argc, char** argv) {
    if (argc > 1) return child(argv[1]);
    int fds[2];
    tty_size_t size = { 40, 10 };

    check(sys_pty_open(0, &size) == -EFAULT && sys_pty_open(fds, 0) == -EFAULT, "bad pointers give EFAULT");
    tty_size_t bad[] = { { 0, 10 }, { 40, 0 }, { 1001, 10 }, { 40, -1 } };
    int all = 1;
    for (int i = 0; i < 4; i++) all &= sys_pty_open(fds, &bad[i]) == -EINVAL;
    check(all, "a size of nothing or over 1000 is EINVAL");

    check(sys_pty_open(fds, &size) == 0, "open a pty");
    int m = fds[0], s = fds[1];
    check(sys_fdtype(m) == FDTYPE_PTY && sys_fdtype(s) == FDTYPE_TTY, "a master and a terminal");
    tty_size_t z1 = { 0, 0 }, z2 = { 0, 0 };
    check(sys_tty_size(m, &z1) == 0 && sys_tty_size(s, &z2) == 0 && z1.cols == 40 && z1.rows == 10 &&
              z2.cols == 40 && z2.rows == 10,
          "both ends have its size");
    check(events(m, POLLIN | POLLOUT) == POLLOUT, "the master: nothing to read, room to write");
    check(events(s, POLLIN | POLLOUT) == POLLOUT, "the slave: the same");
    char buf[64];
    check(sys_read(s, buf, sizeof buf) == -EIO, "a program not in the pty's foreground cannot read it");
    check(sys_write(s, "hi", 2) == 2 && expect(m, "hi") && got_len == 2, "but can write to it");

    // Lines, as a terminal cooks them.
    int k = start_fg(s, "cat");
    check(k > 0, "start a program on the pty, in its foreground");
    check(start_fg(s, "cat") == -EPERM, "a second may not take the foreground from it");
    put(m, "hello\n");
    check(expect(m, "got:hello\n") && !strncmp(got, "hello\n", 6), "a line typed is echoed, then read");
    put(m, "abX\bc\n");
    check(expect(m, "got:abc\n") && has(got, "X\b \b"), "backspace takes a character back, on screen too");
    put(m, "quit\n");
    check(sys_join(k) == 0, "and the program ends");

    // Ctrl+D at the start of a line.
    k = start_fg(s, "eof");
    check(k > 0, "the pty is free again: another program takes it");
    sys_sleep(1);
    put(m, "\x04");
    check(sys_join(k) == 0, "Ctrl+D is end of file");

    // Raw mode: keys as bytes, the cursor keys as sequences, no echo.
    k = start_fg(s, "raw");
    check(expect(m, "ready\n"), "raw mode");
    char up = (char)KEY_NAV(NAV_UP, 0);
    sys_write(m, &up, 1);
    check(expect(m, "|\n") && !strcmp(got, "1b.5b.41.|\n"), "the up arrow reads as ESC [ A, unechoed");
    check(sys_join(k) == 0, "raw program done");

    // Ctrl+C goes to the pty's foreground job.
    k = start_fg(s, "sleep");
    check(expect(m, "ready\n"), "a program waits");
    put(m, "\x03");
    check(sys_join(k) == EXIT_SIGNAL(SIGINT), "Ctrl+C at the master interrupts it, not this program");

    // The size, sys_print, colours and /dev/tty.
    k = start_fg(s, "info");
    check(k > 0 && sys_join(k) == 0, "info program");
    check(expect(m, "devtty\n"), "a program on the pty:");
    check(has(got, "size 40x10\n") != 0, "its terminal has the pty's size");
    check(has(got, "printed\n") != 0, "sys_print goes to the pty, its terminal");
    check(has(got, "\x1b[0;93;44m") != 0, "a colour arrives as an escape sequence");
    check(has(got, "printed\n\x1b") && has(got, "m" "devtty\n"), "and /dev/tty is the pty");

    // The master reads end of file once no one has the slave open.
    sys_close(s);
    check(sys_read(m, buf, sizeof buf) == 0 && events(m, POLLIN) == (POLLIN | POLLHUP),
          "with every slave closed the master reads end of file, and polls as hung up");
    sys_close(m);

    // The hangup: closing the master signals the programs on the pty.
    check(sys_pty_open(fds, &size) == 0, "another pty");
    m = fds[0], s = fds[1];
    k = start_fg(s, "hup");
    sys_close(s);
    check(expect(m, "ready\n"), "a program that catches SIGHUP");
    sys_close(m);
    check(sys_join(k) == 0, "when the master closes: SIGHUP, reads end of file, writes fail");

    check(sys_pty_open(fds, &size) == 0, "and another");
    m = fds[0], s = fds[1];
    k = start_fg(s, "sleep");
    sys_close(s);
    check(expect(m, "ready\n"), "a program that does not");
    sys_close(m);
    check(sys_join(k) == EXIT_SIGNAL(SIGHUP), "SIGHUP ends it");

    // Everything is let go: as many as the descriptors hold, twice.
    int opened = 0, again = 1;
    for (int round = 0; round < 2; round++) {
        int fd[MAX_FDS], n = 0;
        while (n + 2 <= MAX_FDS - 3 && sys_pty_open(fd + n, &size) == 0) n += 2;
        if (round == 0) opened = n;
        else again = n == opened;
        for (int i = 0; i < n; i++) sys_close(fd[i]);
    }
    check(opened >= 6 && again, "ptys are freed when both ends close");
    return check_summary("ptytest");
}

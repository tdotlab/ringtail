// Channels: names, connecting and accepting, whole messages, the queue
// limit, the end of a channel from either side, the backlog, and a
// conversation with a child (this program again, with a role).
#include "ulib.h"

static int events(int fd, int want) {
    pollfd_t p = { fd, (short)want, 0 };
    return sys_pollfd(&p, 1, 0) < 0 ? -1 : p.revents;
}

static int child(const char* role) {
    if (!strcmp(role, "echo")) {  // connect, say ping, expect pong
        int c = sys_chan_connect("chantest.echo");
        if (c < 0) return 2;
        if (sys_write(c, "ping", 4) != 4) return 3;
        char buf[16];
        int n = sys_read(c, buf, sizeof buf);
        return n == 4 && !memcmp(buf, "pong", 4) ? 0 : 4;
    }
    if (!strcmp(role, "flood")) {  // more than the queue holds, then go
        int c = sys_chan_connect("chantest.flood");
        if (c < 0) return 2;
        for (int i = 0; i < CHAN_QUEUE + 4; i++) {
            char m = (char)('A' + i);
            if (sys_write(c, &m, 1) != 1) return 3;
        }
        return 0;
    }
    return 99;
}

int main(int argc, char** argv) {
    if (argc > 1) return child(argv[1]);
    char buf[CHAN_MSG_MAX + 8];

    char longest[CHAN_NAME_MAX + 2];
    memset(longest, 'n', sizeof longest);
    longest[CHAN_NAME_MAX + 1] = 0;
    check(sys_chan_listen("") == -EINVAL && sys_chan_listen("a/b") == -EINVAL, "names may not be empty or hold '/'");
    check(sys_chan_listen(longest) == -EINVAL, "nor be longer than CHAN_NAME_MAX");
    check(sys_chan_listen(0) == -EFAULT && sys_chan_connect((char*)0xC0000000ull) == -EFAULT, "bad pointers give EFAULT");
    longest[CHAN_NAME_MAX] = 0;
    int l = sys_chan_listen(longest);
    check(l >= 0 && sys_fdtype(l) == FDTYPE_CHAN, "listen on the longest name");
    check(sys_chan_listen(longest) == -EADDRINUSE, "a name has one listener");
    sys_close(l);
    l = sys_chan_listen(longest);
    check(l >= 0, "the name is free again when the listener closes");
    sys_close(l);

    check(sys_chan_connect("chantest.nobody") == -ECONNREFUSED, "connecting to nobody is refused");
    l = sys_chan_listen("chantest");
    check(events(l, POLLIN) == 0, "a listener with nothing waiting is not readable");
    check(sys_accept(l, 0, 0) == -EAGAIN, "accept with no wait gives EAGAIN");
    uint32_t t0 = sys_ticks();
    check(sys_accept(l, 0, 50) == -ETIMEDOUT && sys_ticks() - t0 >= 4, "accept with a timeout waits that long");
    int c = sys_chan_connect("chantest");
    check(c >= 0 && sys_fdtype(c) == FDTYPE_CHAN, "connect");
    check(events(l, POLLIN) == POLLIN, "the listener is readable while a connection waits");
    check(sys_write(c, "early", 5) == 5, "a channel can be written before it is accepted");
    int s = sys_accept(l, 0, -1);
    check(s >= 0 && sys_fdtype(s) == FDTYPE_CHAN, "accept it");
    check(sys_read(s, buf, sizeof buf) == 5 && !memcmp(buf, "early", 5), "and what was written comes out");

    check(sys_write(c, "ab", 2) == 2 && sys_write(c, "cde", 3) == 3, "two messages");
    check(sys_read(s, buf, sizeof buf) == 2 && sys_read(s, buf + 2, sizeof buf) == 3 && !memcmp(buf, "abcde", 5),
          "come out as two reads, not run together");
    check(sys_write(s, "0123456789", 10) == 10 && sys_read(c, buf, 4) == 4 && !memcmp(buf, "0123", 4),
          "a short read gets the start of a message, the other way too");
    check(events(c, POLLIN) == 0, "and the rest is dropped");
    memset(buf, 'm', sizeof buf);
    check(sys_write(c, buf, CHAN_MSG_MAX) == CHAN_MSG_MAX, "a message of CHAN_MSG_MAX bytes");
    check(sys_read(s, buf, sizeof buf) == CHAN_MSG_MAX, "arrives whole");
    check(sys_write(c, buf, CHAN_MSG_MAX + 1) == -EMSGSIZE, "a longer one is refused");
    check(sys_write(c, buf, 0) == 0 && events(s, POLLIN) == 0, "writing nothing sends nothing");
    check(sys_write(c, (void*)0xC0000000ull, 4) == -EFAULT && sys_read(s, (void*)0, 4) == -EFAULT,
          "bad buffers give EFAULT");
    check(events(s, POLLIN) == 0, "and send nothing");

    int n = 0;
    while (events(c, POLLOUT) == POLLOUT && n < 100) {
        char m = (char)n;
        sys_write(c, &m, 1);
        n++;
    }
    check(n == CHAN_QUEUE, "a direction holds CHAN_QUEUE messages, and POLLOUT says when there is room");
    check(events(s, POLLIN | POLLOUT) == (POLLIN | POLLOUT), "the other way is unaffected");
    int order = 1;
    for (int i = 0; i < n; i++) order &= sys_read(s, buf, 1) == 1 && buf[0] == (char)i;
    check(order, "they come out in order");

    check(sys_read(l, buf, 1) == -EBADF && sys_write(l, "x", 1) == -EBADF, "a listener is not read or written");
    check(sys_seek(c, 0, SEEK_SET) == -EINVAL, "channels do not seek");
    check(sys_accept(c, 0, 0) == -ENOTSOCK, "nor accept");

    sys_write(c, "last", 4);
    sys_close(c);
    check(events(s, POLLIN) == (POLLIN | POLLHUP), "the other end closing shows as POLLIN and POLLHUP");
    check(sys_read(s, buf, sizeof buf) == 4 && sys_read(s, buf, sizeof buf) == 0,
          "messages already sent are read, then end of file");
    check(sys_write(s, "x", 1) == -EPIPE, "writing to a closed channel gives EPIPE, and no SIGPIPE");
    sys_close(s);

    // The backlog, and a listener closed with connections waiting.
    int w[CHAN_BACKLOG + 1];
    int made = 0;
    while (made <= CHAN_BACKLOG && (w[made] = sys_chan_connect("chantest")) >= 0) made++;
    check(made == CHAN_BACKLOG && w[made] == -ECONNREFUSED, "at most CHAN_BACKLOG connections wait");
    sys_close(w[0]);
    s = sys_accept(l, 0, 0);
    check(s >= 0 && sys_read(s, buf, sizeof buf) == 0, "one closed before it was accepted reads as ended");
    sys_close(s);
    sys_close(l);
    check(sys_read(w[1], buf, sizeof buf) == 0 && sys_write(w[1], "x", 1) == -EPIPE,
          "closing the listener ends the connections still waiting");
    for (int i = 1; i < made; i++) sys_close(w[i]);

    // A conversation with a child.
    l = sys_chan_listen("chantest.echo");
    const char* echo[] = { "chantest", "echo", 0 };
    int k = sys_spawn_args("chantest", echo);
    s = sys_accept(l, 0, 5000);
    n = s >= 0 ? sys_read(s, buf, sizeof buf) : -1;
    check(n == 4 && !memcmp(buf, "ping", 4), "a child connects and sends");
    check(sys_write(s, "pong", 4) == 4 && k > 0 && sys_join(k) == 0, "and gets the answer");
    sys_close(s);
    sys_close(l);

    // A child that writes more than the queue holds waits for room,
    // and its messages outlive it.
    l = sys_chan_listen("chantest.flood");
    const char* flood[] = { "chantest", "flood", 0 };
    k = sys_spawn_args("chantest", flood);
    s = sys_accept(l, 0, 5000);
    sys_sleep(1);
    int full = events(s, POLLIN) == POLLIN;
    int got = 0, r;
    order = 1;
    while ((r = sys_read(s, buf, sizeof buf)) > 0) order &= buf[0] == (char)('A' + got++);
    check(full && got == CHAN_QUEUE + 4 && order && r == 0, "a writer waits for room, and nothing is lost");
    check(k > 0 && sys_join(k) == 0, "the child finished");
    sys_close(s);
    sys_close(l);

    l = sys_chan_listen("chantest");
    check(l >= 0, "every name was let go");
    sys_close(l);
    return check_summary("chantest");
}

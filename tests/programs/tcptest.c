// TCP over loopback: connecting and accepting, data both ways and in
// bulk, the end of a stream, resets, the backlog, polling, and the
// errors for everything done wrong.
//
//   tcptest            the self-check
//   tcptest echo PORT  take one connection on PORT and send back all
//                      that comes, until the other end finishes (for
//                      the tests run from the host)
#include "ulib.h"

#define LO 0x7F000001u

static uint8_t pattern(uint32_t seed, uint32_t i) { return (uint8_t)(seed + i * 7 + (i >> 9)); }

static int listener(int* port, int backlog) {
    int l = sys_socket(SOCK_STREAM);
    *port = sys_bind(l, 0);
    sys_listen(l, backlog);
    return l;
}

static int connect_to(uint32_t ip, int port, int timeout_ms) {
    int fd = sys_socket(SOCK_STREAM);
    if (fd < 0) return fd;
    sock_addr_t a = { ip, port };
    int r = sys_connect(fd, &a, timeout_ms);
    if (r < 0) {
        sys_close(fd);
        return r;
    }
    return fd;
}

// A connected pair over loopback: *c connected, *s accepted.
static int pair(int l, int port, int* c, int* s) {
    *c = connect_to(LO, port, 0);
    *s = *c >= 0 ? sys_accept(l, 0, 1000) : -1;
    return *c >= 0 && *s >= 0;
}

static int poll1(int fd, int events, int timeout_ms) {
    pollfd_t p = { fd, events, 0 };
    int r = sys_pollfd(&p, 1, timeout_ms);
    return r < 0 ? r : p.revents;
}

// total bytes each way between a and b at once, the way a program
// would: write when poll says there is room, read when there is data.
// Returns the ticks it took, or -1 if the data came wrong or stalled.
static int bulk(int a, int b, uint32_t total) {
    static uint8_t buf[4096];
    int fd[2] = { a, b };
    uint32_t sent[2] = { 0, 0 }, got[2] = { 0, 0 };
    uint32_t start = (uint32_t)sys_ticks();
    while (got[0] < total || got[1] < total) {
        pollfd_t p[2];
        for (int i = 0; i < 2; i++) p[i] = (pollfd_t){ fd[i], POLLIN | (sent[i] < total ? POLLOUT : 0), 0 };
        if (sys_pollfd(p, 2, 5000) <= 0) return -1;
        for (int i = 0; i < 2; i++) {
            if ((p[i].revents & POLLOUT) && sent[i] < total) {
                uint32_t n = MIN(total - sent[i], sizeof buf);
                for (uint32_t k = 0; k < n; k++) buf[k] = pattern((uint32_t)i, sent[i] + k);
                int r = sys_write(fd[i], buf, n);
                if (r != (int)n) return -1;
                sent[i] += n;
            }
            if (p[i].revents & POLLIN) {
                int r = sys_read(fd[i], buf, sizeof buf);
                if (r <= 0) return -1;
                for (int k = 0; k < r; k++)
                    if (buf[k] != pattern((uint32_t)(1 - i), got[i] + (uint32_t)k)) return -1;
                got[i] += (uint32_t)r;
            }
        }
    }
    return (int)((uint32_t)sys_ticks() - start);
}

static int echo(int argc, char** argv) {
    uint32_t port;
    if (argc != 3 || parse_uint(argv[2], &port) || port == 0 || port > 65535) {
        dprintf(STDERR_FILENO, "usage: tcptest [echo PORT]\n");
        return 2;
    }
    int l = sys_socket(SOCK_STREAM), r = sys_bind(l, (int)port);
    if (r < 0 || (r = sys_listen(l, 1)) < 0) {
        printf("tcptest: port %u: %s\n", port, strerror(r));
        return 1;
    }
    printf("echoing on port %u\n", port);
    sock_addr_t peer;
    int c = sys_accept(l, &peer, -1);
    if (c < 0) {
        printf("tcptest: accept: %s\n", strerror(c));
        return 1;
    }
    sys_close(l);
    printf("connection from " IPV4_FMT ":%d\n", IPV4_ARGS(peer.ip), peer.port);
    static uint8_t buf[4096];
    uint32_t total = 0;
    for (;;) {
        int n = sys_read(c, buf, sizeof buf);
        if (n < 0) {
            printf("tcptest: read: %s\n", strerror(n));
            return 1;
        }
        if (n == 0) break;
        if ((r = sys_write(c, buf, (uint32_t)n)) != n) {
            printf("tcptest: write: %s\n", strerror(r));
            return 1;
        }
        total += (uint32_t)n;
    }
    sys_close(c);
    printf("echoed %u bytes\n", total);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "echo")) return echo(argc, argv);
    if (argc > 1) return echo(0, argv);  // just the usage
    char buf[256];
    int r, port, c, s;

    // ---- Descriptors, and what a socket that is not connected does ----
    int t = sys_socket(SOCK_STREAM);
    check(t == 3, "socket(SOCK_STREAM) gives a descriptor");
    int u = sys_socket(SOCK_DGRAM);
    int pf[2];
    sys_pipe(pf);
    check(sys_fdtype(t) == FDTYPE_TCP && sys_fdtype(u) == FDTYPE_UDP && sys_fdtype(0) == FDTYPE_TTY &&
          sys_fdtype(1) == FDTYPE_TTY && sys_fdtype(pf[0]) == FDTYPE_PIPE && sys_fdtype(15) == -EBADF,
          "fdtype tells them apart");
    check(sys_read(t, buf, 10) == -ENOTCONN && sys_write(t, "x", 1) == -ENOTCONN, "no reading or writing before a connection");
    check(sys_shutdown(t, SHUT_WR) == -ENOTCONN, "nor shutting one down");
    sock_addr_t a;
    check(sys_sockaddr(t, 1, &a) == -ENOTCONN, "and there is no peer");
    check(sys_sockaddr(t, 0, &a) == 0 && a.ip == 0 && a.port == 0, "nor a port yet");
    check(sys_accept(t, 0, 0) == -EINVAL, "accept wants a listening socket");
    udp_msg_t m = { LO, 9, buf, 1, 0 };
    check(sys_sendto(t, &m) == -EOPNOTSUPP && sys_recvfrom(t, &m) == -EOPNOTSUPP, "UDP calls on a TCP socket are not supported");
    a = (sock_addr_t){ LO, 9 };
    check(sys_listen(u, 1) == -EOPNOTSUPP && sys_connect(u, &a, 0) == -EOPNOTSUPP && sys_accept(u, 0, 0) == -EOPNOTSUPP,
          "nor TCP calls on a UDP one");
    check(sys_listen(pf[0], 1) == -ENOTSOCK && sys_connect(1, &a, 0) == -ENOTSOCK, "nor on anything else");
    check(sys_connect(t, (const sock_addr_t*)0xC0000000u, 0) == -EFAULT, "connect checks its pointer");
    a = (sock_addr_t){ LO, 0 };
    check(sys_connect(t, &a, 0) == -EINVAL, "port 0 is nowhere to connect to");
    a = (sock_addr_t){ 0xE0000001u, 80 };
    check(sys_connect(t, &a, 0) == -EINVAL, "nor is a multicast address");
    a = (sock_addr_t){ LO, 1 };
    check(sys_connect(t, &a, 0) == -ECONNREFUSED, "a port nobody listens on refuses");
    check(sys_connect(t, &a, 0) == -ECONNREFUSED, "and a socket may try again");
    sys_close(t);
    sys_close(u);

    // ---- Listening ----
    int l = listener(&port, 4);
    check(port >= 49152 && port <= 65535, "bind 0 picks a port from 49152 up");
    check(sys_sockaddr(l, 0, &a) == 0 && a.ip == 0 && a.port == port, "a listener's address is its port on any address");
    t = sys_socket(SOCK_STREAM);
    check(sys_bind(t, port) == -EADDRINUSE, "a second socket cannot have the port");
    check(sys_bind(t, 70000) == -EINVAL, "nor a port past 65535");
    sys_close(t);
    check(sys_accept(l, 0, 0) == -EAGAIN, "accept with no wait when nobody has come");
    uint32_t t0 = (uint32_t)sys_ticks();
    r = sys_accept(l, 0, 100);
    uint32_t took = (uint32_t)sys_ticks() - t0;
    check(r == -ETIMEDOUT && took >= 9 && took <= 30, "and with a wait that runs out");
    check(poll1(l, POLLIN, 0) == 0, "poll: nothing to accept");

    // ---- A connection ----
    c = connect_to(LO, port, 0);
    check(c >= 0, "connect");
    check(poll1(l, POLLIN, 1000) == POLLIN, "poll: now there is");
    sock_addr_t peer = { 0, 0 };
    s = sys_accept(l, &peer, 1000);
    check(s >= 0, "accept");
    sock_addr_t ca, cp, sa, sp;
    sys_sockaddr(c, 0, &ca);
    sys_sockaddr(c, 1, &cp);
    sys_sockaddr(s, 0, &sa);
    sys_sockaddr(s, 1, &sp);
    check(peer.ip == LO && peer.port == ca.port && sp.ip == LO && sp.port == ca.port, "accept names the peer, as sockaddr does");
    check(cp.ip == LO && cp.port == port && sa.ip == LO && sa.port == port && ca.port >= 49152,
          "both ends agree on the addresses");
    a = (sock_addr_t){ LO, port };
    check(sys_connect(c, &a, 0) == -EISCONN, "a connected socket cannot connect again");
    check(sys_listen(c, 1) == -EINVAL, "nor listen");
    check(poll1(s, POLLIN | POLLOUT, 0) == POLLOUT, "poll: room to write, nothing to read");
    check(sys_write(c, "hello", 5) == 5, "write");
    check(poll1(s, POLLIN, 1000) == POLLIN, "poll: something to read");
    memset(buf, 0, sizeof buf);
    check(sys_read(s, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5), "read gets it");
    check(sys_write(s, "and back", 8) == 8 && sys_read(c, buf, sizeof buf) == 8 && !memcmp(buf, "and back", 8),
          "and the other way");
    sys_write(c, "abcdef", 6);
    while (poll1(s, POLLIN, 1000) == POLLIN && sys_read(s, buf, 2) == 2 && buf[0] != 'e') { }
    check(buf[0] == 'e' && buf[1] == 'f', "a read takes what fits and leaves the rest");

    r = bulk(c, s, 256 * 1024);
    check(r >= 0, "256 KB each way at once, intact");
    if (r >= 0) printf("  (%u KB/s each way over loopback)\n", 256 * 100 / (uint32_t)(r ? r : 1));

    // ---- The end ----
    check(sys_shutdown(c, SHUT_WR) == 0, "shutdown for writing");
    check(sys_read(s, buf, sizeof buf) == 0, "the other end reads the end");
    check(sys_read(s, buf, sizeof buf) == 0, "and again");
    check(poll1(s, POLLIN, 0) == POLLIN, "poll: the end is something to read");
    check(sys_write(c, "x", 1) == -EPIPE, "no writing after it");
    check(sys_write(s, "still", 5) == 5 && sys_read(c, buf, sizeof buf) == 5, "the other way still works");
    sys_close(s);
    check(sys_read(c, buf, sizeof buf) == 0, "until that end closes too");
    check(poll1(c, POLLIN, 0) & POLLHUP, "poll: and nothing more will come either way");
    sys_close(c);

    // ---- Resets ----
    pair(l, port, &c, &s);
    sys_write(s, "unread", 6);
    poll1(c, POLLIN, 1000);
    sys_close(c);  // with data unread: a reset
    check(poll1(s, POLLIN, 1000) & POLLERR, "closing with data unread resets the connection");
    check(sys_read(s, buf, sizeof buf) == -ECONNRESET, "the other end reads that");
    check(sys_read(s, buf, sizeof buf) == 0, "once; then it reads as over");
    sys_close(s);

    pair(l, port, &c, &s);
    t = sys_dup2(c, 9);
    sys_close(c);
    check(sys_write(t, "dup", 3) == 3 && sys_read(s, buf, sizeof buf) == 3, "a copy of the descriptor is the same connection");
    sys_close(t);
    check(sys_read(s, buf, sizeof buf) == 0, "closed with the last copy");
    sys_close(s);

    // ---- The backlog ----
    sys_close(l);
    l = listener(&port, 1);
    c = connect_to(LO, port, 0);
    t0 = (uint32_t)sys_ticks();
    t = connect_to(LO, port, 300);
    took = (uint32_t)sys_ticks() - t0;
    check(c >= 0 && t == -ETIMEDOUT && took >= 29, "with the backlog full a connection waits");
    s = sys_accept(l, 0, 0);
    t = connect_to(LO, port, 3000);
    check(s >= 0 && t >= 0, "and gets in once there is room");
    sys_close(l);
    check(sys_read(t, buf, sizeof buf) == -ECONNRESET, "closing the listener resets what was not accepted");
    check(sys_write(s, "on", 2) == 2 && sys_read(c, buf, sizeof buf) == 2, "but not what was");
    sys_close(t);
    sys_close(s);
    sys_close(c);

    // ---- Many at once ----
    l = listener(&port, 8);
    // Five pairs fill the descriptor table, with the pipe still open.
    int cs[5], ss[5], ok = 1;
    for (int i = 0; i < 5; i++) ok &= pair(l, port, &cs[i], &ss[i]);
    for (int i = 0; i < 5 && ok; i++) {
        char msg[16];
        int n = snprintf(msg, sizeof msg, "conn %d", i);
        ok &= sys_write(cs[i], msg, (uint32_t)n) == n;
    }
    for (int i = 4; i >= 0 && ok; i--) {
        char want[16];
        int n = snprintf(want, sizeof want, "conn %d", i);
        ok &= sys_read(ss[i], buf, sizeof buf) == n && !memcmp(buf, want, (size_t)n);
    }
    check(ok, "five connections at once keep their data apart");
    check(sys_socket(SOCK_STREAM) == -EMFILE && sys_accept(l, 0, 0) == -EMFILE, "and a full table of descriptors is an error");
    for (int i = 0; i < 5; i++) { sys_close(cs[i]); sys_close(ss[i]); }

    // To our own address, if we have one: it goes round inside too.
    netconf_t nc;
    if (sys_netconf(NETCONF_GET, &nc) == 0 && nc.ip) {
        c = connect_to(nc.ip, port, 1000);
        s = c >= 0 ? sys_accept(l, &peer, 1000) : -1;
        check(s >= 0 && peer.ip == nc.ip && sys_write(c, "me", 2) == 2 && sys_read(s, buf, sizeof buf) == 2,
              "a connection to our own address");
        sys_close(c);
        sys_close(s);
    }
    sys_close(l);

    // ---- pollfd itself ----
    pollfd_t p[3] = { { pf[0], POLLIN, 0 }, { pf[1], POLLOUT, 0 }, { -1, POLLIN, 77 } };
    check(sys_pollfd(p, 3, 0) == 1 && p[0].revents == 0 && p[1].revents == POLLOUT && p[2].revents == 0,
          "pollfd: an empty pipe has room, and a negative fd is skipped");
    sys_write(pf[1], "p", 1);
    check(sys_pollfd(p, 2, 0) == 2 && p[0].revents == POLLIN, "a pipe with something in it is readable");
    sys_close(pf[1]);
    p[1].fd = pf[1];
    check(sys_pollfd(p, 2, 0) == 2 && p[0].revents == (POLLIN | POLLHUP) && p[1].revents == POLLNVAL,
          "a pipe with no writer has hung up; a closed fd is not valid");
    sys_close(pf[0]);
    t = sys_socket(SOCK_DGRAM);
    sys_bind(t, 0);
    t0 = (uint32_t)sys_ticks();
    r = poll1(t, POLLIN, 120);
    took = (uint32_t)sys_ticks() - t0;
    check(r == 0 && took >= 11 && took <= 40, "nothing ready: pollfd waits the time out");
    sys_close(t);
    check(sys_pollfd(p, MAX_FDS + 1, 0) == -EINVAL && sys_pollfd((pollfd_t*)0xC0000000u, 1, 0) == -EFAULT,
          "too many descriptors, or a bad pointer");
    check(sys_sockaddr(3, 0, &a) == -EBADF && sys_shutdown(3, SHUT_RDWR) == -EBADF, "sockaddr and shutdown on nothing");
    return check_summary("tcptest");
}

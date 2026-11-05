// UDP sockets, over loopback: binding, sending and receiving, timeouts,
// the queue limit, and the errors for everything done wrong.
//
//   udptest              the self-check
//   udptest echo PORT N  send the next N datagrams to PORT back where
//                        they came from, then exit (for the peer tests)
#include "ulib.h"

#define LO 0x7F000001u

static int send_to(int fd, uint32_t ip, int port, const void* buf, int len) {
    udp_msg_t m = { ip, port, (void*)buf, len, 0 };
    return sys_sendto(fd, &m);
}

static int recv_from(int fd, void* buf, int size, int timeout_ms, udp_msg_t* out) {
    udp_msg_t m = { 0, 0, buf, size, timeout_ms };
    int r = sys_recvfrom(fd, &m);
    if (out) *out = m;
    return r;
}

static int echo(int argc, char** argv) {
    uint32_t port, n;
    if (argc != 4 || parse_uint(argv[2], &port) || parse_uint(argv[3], &n)) {
        dprintf(STDERR_FILENO, "usage: udptest [echo PORT COUNT]\n");
        return 2;
    }
    int fd = sys_socket(SOCK_DGRAM);
    int r = sys_bind(fd, (int)port);
    if (r < 0) {
        printf("udptest: bind %u: %s\n", port, strerror(r));
        return 1;
    }
    printf("echoing %u datagrams on port %u\n", n, port);
    static uint8_t buf[UDP_MAX_SIZE];
    for (uint32_t i = 0; i < n; i++) {
        udp_msg_t m;
        if ((r = recv_from(fd, buf, sizeof buf, -1, &m)) < 0) {
            printf("udptest: %s\n", strerror(r));
            return 1;
        }
        printf("%d bytes from " IPV4_FMT ":%d\n", r, IPV4_ARGS(m.ip), m.port);
        if ((r = send_to(fd, m.ip, m.port, buf, r)) < 0) printf("udptest: send back: %s\n", strerror(r));
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "echo")) return echo(argc, argv);
    if (argc > 1) return echo(0, argv);  // just the usage
    char buf[2000];
    udp_msg_t m;

    int a = sys_socket(SOCK_DGRAM), b = sys_socket(SOCK_DGRAM);
    check(a == 3 && b == 4, "socket gives descriptors");
    check(sys_socket(3) == -EINVAL && sys_socket(0) == -EINVAL, "SOCK_DGRAM and SOCK_STREAM are the socket types");
    check(recv_from(a, buf, 10, 0, 0) == -EINVAL, "an unbound socket has nothing to receive");
    int pa = sys_bind(a, 0);
    check(pa >= 49152 && pa <= 65535, "bind 0 picks a free port from 49152 up");
    check(sys_bind(a, 0) == -EINVAL && sys_bind(a, 4000) == -EINVAL, "a socket is bound once");
    check(sys_bind(b, pa) == -EADDRINUSE, "a port is one socket's");
    check(sys_bind(b, 68) == -EADDRINUSE, "68 belongs to DHCP");
    check(sys_bind(b, -1) == -EINVAL && sys_bind(b, 65536) == -EINVAL, "ports go from 0 to 65535");
    check(sys_bind(b, 4000) == 4000, "bind to a port of one's choosing");
    check(sys_bind(1, 0) == -ENOTSOCK && sys_bind(9, 0) == -EBADF, "bind wants a socket");

    check(send_to(b, LO, pa, "hello", 5) == 5, "send over loopback");
    int r = recv_from(a, buf, sizeof buf, 1000, &m);
    check(r == 5 && !memcmp(buf, "hello", 5), "it arrives whole");
    check(m.ip == LO && m.port == 4000, "with where it came from");
    check(send_to(a, 0x7F000005u, 4000, "x", 1) == 1 && recv_from(b, buf, 8, 1000, &m) == 1,
          "any 127 address is this machine");
    check(send_to(b, LO, pa, "", 0) == 0 && recv_from(a, buf, sizeof buf, 1000, 0) == 0, "an empty datagram");
    check(send_to(b, LO, pa, "0123456789", 10) == 10 && recv_from(a, buf, 4, 1000, 0) == 4 &&
          !memcmp(buf, "0123", 4), "a short buffer gets the start");
    check(recv_from(a, buf, sizeof buf, 0, 0) == -EAGAIN, "and the rest is gone");

    uint32_t t0 = sys_ticks();
    r = recv_from(a, buf, sizeof buf, 150, 0);
    uint32_t took = sys_ticks() - t0;
    check(r == -ETIMEDOUT && took >= 14 && took <= 30, "a timeout times out, on time");

    memset(buf, 'z', sizeof buf);
    check(send_to(b, LO, pa, buf, UDP_MAX_SIZE) == UDP_MAX_SIZE, "the largest datagram goes");
    check(recv_from(a, buf, sizeof buf, 1000, 0) == UDP_MAX_SIZE, "and comes");
    check(send_to(b, LO, pa, buf, UDP_MAX_SIZE + 1) == -EMSGSIZE, "one byte more is too big");
    check(send_to(b, LO, 0, "x", 1) == -EINVAL && send_to(b, LO, 65536, "x", 1) == -EINVAL, "no port 0 or past 65535");
    check(send_to(b, 0, pa, "x", 1) == -EINVAL && send_to(b, 0xE0000001u, pa, "x", 1) == -EINVAL &&
          send_to(b, 0xF0000001u, pa, "x", 1) == -EINVAL, "no 0.0.0.0, multicast or class E");
    check(send_to(b, LO, pa, "x", -1) == -EINVAL, "no negative length");

    // The queue holds 16; the rest are dropped, not waited for.
    int sent = 0;
    for (int i = 0; i < 20; i++) sent += send_to(b, LO, pa, &i, sizeof i) == (int)sizeof i;
    sys_poll(5);  // loopback goes through the net task
    int got = 0, order = 1, v;
    while (recv_from(a, &v, sizeof v, 200, 0) == (int)sizeof v) order &= v == got++;
    check(sent == 20 && got == 16 && order, "16 wait in order, the rest are lost");

    check(sys_sendto(b, (udp_msg_t*)0) == -EFAULT && sys_recvfrom(a, (udp_msg_t*)0xC0000000u) == -EFAULT,
          "a bad message pointer gives EFAULT");
    check(send_to(b, LO, pa, (void*)0xC0000000u, 4) == -EFAULT, "a bad buffer to send gives EFAULT");
    send_to(b, LO, pa, "keep", 4);
    sys_poll(5);
    check(recv_from(a, (void*)main, 4, 0, 0) == -EFAULT, "receiving into code gives EFAULT");
    check(recv_from(a, buf, 4, 0, 0) == 4 && !memcmp(buf, "keep", 4), "and loses nothing");
    check(sys_read(a, buf, 4) == -EINVAL && sys_write(a, "x", 1) == -EINVAL, "sockets are not read or written");
    check(sys_sendto(1, &m) == -ENOTSOCK && sys_recvfrom(MAX_FDS, &m) == -EBADF, "sendto and recvfrom want sockets");

    check(sys_dup2(a, 7) == 7, "dup2 copies a socket");
    sys_close(a);
    check(send_to(b, LO, pa, "dup", 3) == 3 && recv_from(7, buf, 8, 1000, 0) == 3, "which lives on in the copy");
    sys_close(7);
    int c = sys_socket(SOCK_DGRAM);
    check(sys_bind(c, pa) == pa, "the last close frees the port");
    check(send_to(b, LO, 9, "x", 1) == 1, "sending to a closed port is not an error here");
    sys_close(b);
    sys_close(c);

    int opened = 0;
    while ((r = sys_socket(SOCK_DGRAM)) >= 0) opened++;
    check(r == -EMFILE && opened == MAX_FDS - 3, "sockets stop at the descriptor limit");
    for (int fd = 3; fd < MAX_FDS; fd++) sys_close(fd);

    netconf_t n;
    check(sys_netconf(NETCONF_GET, &n) == 0, "netconf reads the settings");
    check(sys_netconf(NETCONF_GET, (netconf_t*)main) == -EFAULT && sys_netconf(9, &n) == -EINVAL,
          "and wants a writable struct and a known request");
    return check_summary("udptest");
}

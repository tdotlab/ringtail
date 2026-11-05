// nc [-C] [-v] [-w SECS] HOST PORT    connect to HOST on PORT
// nc -l [-C] [-v] PORT                wait for one connection on PORT
//
// Then copy what comes in on the connection to the screen, and what is
// typed (or what comes in on stdin) to the connection. On the terminal
// a line goes when Enter is pressed, and Ctrl+D on an empty line ends
// the sending side; nc exits when the other end has finished sending,
// or on Ctrl+C. -C ends lines with CR LF, as HTTP and SMTP want them.
// -w gives up on connecting after SECS seconds (TCP itself waits 31).
#include "ulib.h"
#include "dns.h"
#include "dial.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: nc [-C] [-v] [-w SECS] HOST PORT\n"
                           "       nc -l [-C] [-v] PORT\n");
    return 2;
}

static int crlf, sock;
static char line[256];  // typed and not yet sent
static uint32_t len;

static int send_all(const char* p, uint32_t n) {
    int r = n ? sys_write(sock, p, n) : 0;
    if (r < 0) dprintf(STDERR_FILENO, "\nnc: %s\n", strerror(r));
    return r < 0 ? r : 0;
}

// The terminal, raw: keys arrive one by one, and the line being typed
// is ours to echo and edit, so that what comes from the other end can
// be printed above it without the two getting mixed up.
static int key(int c, int* done) {
    static int esc;  // inside an escape sequence: skip it
    if (esc) {
        if (esc == 1 && c != '[' && c != 'O') esc = 0;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') esc = 0;
        else esc = 2;
        return 0;
    }
    if (c == 0x1B) { esc = 1; return 0; }
    if (c == '\r' || c == '\n') {
        print("\n");
        if (crlf) line[len++] = '\r';
        line[len++] = '\n';
        int r = send_all(line, len);
        len = 0;
        return r;
    }
    if (c == 4) {  // Ctrl+D
        if (len) {  // as in a cooked terminal: the line goes, with no end
            int r = send_all(line, len);
            len = 0;
            print("\n");
            return r;
        }
        sys_shutdown(sock, SHUT_WR);
        *done = 1;
        return 0;
    }
    if (c == 0x7F || c == '\b') {
        if (len) { len--; print("\b \b"); }
        return 0;
    }
    if (((c >= 0x20 && c < 0x7F) || c == '\t') && len < sizeof line - 2) {
        line[len++] = (char)c;
        char ch = (char)c;
        sys_write(STDOUT_FILENO, &ch, 1);
    }
    return 0;
}

int main(int argc, char** argv) {
    int listen = 0, verbose = 0;
    uint32_t wait_s = 0;
    const char* args[2];
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) listen = 1;
        else if (!strcmp(argv[i], "-C")) crlf = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-w")) {
            if (++i == argc || parse_uint(argv[i], &wait_s) || wait_s == 0 || wait_s > 3600) return usage();
        } else if (argv[i][0] == '-' || nargs == 2) return usage();
        else args[nargs++] = argv[i];
    }
    uint32_t port;
    if (nargs != (listen ? 1 : 2) || parse_uint(args[nargs - 1], &port) || port == 0 || port > 65535) return usage();

    if (listen) {
        int l = sys_socket(SOCK_STREAM), r = l;
        if (l >= 0) r = sys_bind(l, (int)port);
        if (r >= 0) r = sys_listen(l, 1);
        if (r < 0) {
            dprintf(STDERR_FILENO, "nc: port %u: %s\n", port, strerror(r));
            return 1;
        }
        if (verbose) dprintf(STDERR_FILENO, "Listening on port %u\n", port);
        sock_addr_t peer;
        sock = sys_accept(l, &peer, -1);
        sys_close(l);
        if (sock < 0) {
            if (sock != -EINTR) dprintf(STDERR_FILENO, "nc: %s\n", strerror(sock));
            return 1;
        }
        if (verbose) dprintf(STDERR_FILENO, "Connection from " IPV4_FMT ":%d\n", IPV4_ARGS(peer.ip), peer.port);
    } else {
        uint32_t ip = 0;
        sock = dial(args[0], (int)port, (int)wait_s * 1000, &ip);
        if (sock < 0) {
            if (sock == -EINTR) return 1;
            if (ip) dprintf(STDERR_FILENO, "nc: " IPV4_FMT " port %u: %s\n", IPV4_ARGS(ip), port, dns_strerror(sock));
            else dprintf(STDERR_FILENO, "nc: %s: %s\n", args[0], dns_strerror(sock));
            return 1;
        }
        if (verbose) dprintf(STDERR_FILENO, "Connected to %s (" IPV4_FMT ") port %u\n", args[0], IPV4_ARGS(ip), port);
    }

    // Typing needs the terminal raw, and ours: a job in the background
    // just listens.
    int tty = sys_fdtype(STDIN_FILENO) == FDTYPE_TTY;
    int old_mode = tty ? sys_tty_mode(TTY_RAW) : 0;
    int in_open = !tty || old_mode >= 0;
    static char buf[4096];
    int status = 0;
    for (;;) {
        pollfd_t p[2] = { { sock, POLLIN, 0 }, { in_open ? STDIN_FILENO : -1, POLLIN, 0 } };
        int r = sys_pollfd(p, 2, -1);
        if (r < 0) { status = 1; break; }
        if (p[0].revents) {
            int n = sys_read(sock, buf, sizeof buf);
            if (n <= 0) {
                if (n < 0) {
                    dprintf(STDERR_FILENO, "%snc: %s\n", len ? "\n" : "", strerror(n));
                    status = 1;
                }
                break;
            }
            // Above the line being typed: take it off, print, put it back.
            if (len) print("\r\x1b[K");
            sys_write(STDOUT_FILENO, buf, (uint32_t)n);
            if (len) sys_write(STDOUT_FILENO, line, len);
        }
        if (p[1].revents) {
            int n = sys_read(STDIN_FILENO, buf, sizeof buf);
            int done = n <= 0;
            if (n > 0 && tty)
                for (int i = 0; i < n && !done; i++)
                    if (key((uint8_t)buf[i], &done) < 0) { status = 1; break; }
            if (n > 0 && !tty && send_all(buf, (uint32_t)n) < 0) status = 1;
            if (status) break;
            if (done) {
                if (!tty) sys_shutdown(sock, SHUT_WR);
                in_open = 0;
            }
        }
    }
    if (tty && old_mode >= 0) sys_tty_mode(old_mode);
    if (len) print("\n");
    sys_close(sock);
    return status;
}

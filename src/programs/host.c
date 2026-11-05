// host [-p PORT] NAME [SERVER]: ask DNS about NAME and say what came
// back, in the words BIND's host uses. NAME as a dotted address asks
// for its name instead. SERVER is an address; without one, the servers
// the network was given (see `net`). PORT is for a server that does
// not listen on 53, like a test one.
#include "ulib.h"
#include "dns.h"

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: host [-p PORT] NAME [SERVER]\n"
                           "  NAME    a name to look up, or an address to find the name of\n"
                           "  SERVER  the DNS server's address (default: the ones `net` shows)\n");
    return 2;
}

// "a is an alias for b." for each step; returns the name at the end.
static const char* print_aliases(const dns_result_t* r) {
    const char* from = r->name;
    for (int i = 0; i < r->a.nchain; from = r->a.chain[i++]) printf("%s is an alias for %s.\n", from, r->a.chain[i]);
    return from;
}

int main(int argc, char** argv) {
    const char *name = 0, *server_arg = 0;
    uint32_t port = 0, server = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) {
            if (++i == argc || parse_uint(argv[i], &port) || port < 1 || port > 65535) return usage();
        } else if (argv[i][0] == '-') return usage();
        else if (!name) name = argv[i];
        else if (!server_arg) server_arg = argv[i];
        else return usage();
    }
    if (!name) return usage();
    if (server_arg && (parse_ipv4(server_arg, &server) || !server)) {
        dprintf(STDERR_FILENO, "host: the server must be an address like 10.0.2.3, not %s\n", server_arg);
        return 2;
    }

    dns_result_t r;
    uint32_t ip;
    if (parse_ipv4(name, &ip) == 0) {
        int e = dns_reverse(ip, server, (int)port, &r);
        if (e == DNS_E_NODATA) {
            printf("%s has no name\n", r.name);
            return 1;
        }
        if (e) {
            dprintf(STDERR_FILENO, "host: %s: %s\n", name, dns_strerror(e));
            return 1;
        }
        const char* from = print_aliases(&r);
        printf("%s domain name pointer %s.\n", from, r.a.ptr);
        return 0;
    }

    int n = dns_lookup(name, server, (int)port, &r);
    if (n < 0) {
        dprintf(STDERR_FILENO, "host: %s: %s\n", name, dns_strerror(n));
        return 1;
    }
    const char* from = print_aliases(&r);
    for (int i = 0; i < n; i++) printf("%s has address " IPV4_FMT "\n", from, IPV4_ARGS(r.a.addrs[i]));
    return 0;
}

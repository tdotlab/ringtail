// See dial.h.
#include "ulib.h"
#include "dns.h"
#include "dial.h"

int dial(const char* host, int port, int timeout_ms, uint32_t* ip) {
    dns_result_t r;
    int n = dns_lookup(host, 0, 0, &r);
    if (n < 0) return n;
    int err = -ECONNREFUSED;
    for (int i = 0; i < n; i++) {
        if (ip) *ip = r.a.addrs[i];
        int fd = sys_socket(SOCK_STREAM);
        if (fd < 0) return fd;
        sock_addr_t a = { r.a.addrs[i], port };
        err = sys_connect(fd, &a, timeout_ms);
        if (err == 0) return fd;
        sys_close(fd);
        // Worth trying the next address only if this one failed alone.
        if (err == -EINTR || err == -ENETDOWN || err == -EADDRNOTAVAIL || err == -EINVAL) break;
    }
    return err;
}

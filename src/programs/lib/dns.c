// The resolver: see dns.h.
//
// A question goes to each server in turn, twice, with 2 s to answer
// each time. An answer counts only if it comes from that server's
// address and port, carries the query's ID and repeats its question;
// anything else is ignored, so a stranger who wants to slip in a false
// one has to guess the ID and the random port the kernel bound. The ID
// comes from a generator seeded with the clock, the task and that port:
// weak, but the port is the harder half to guess.
#include "ulib.h"
#include "dns.h"

#define TRIES 2
#define WAIT_TICKS 200  // 2 s

typedef struct {
    int fd, port, n;
    uint32_t servers[2];
    char domain[64];
} ctx_t;

static uint32_t rng;

static uint16_t new_id(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint16_t)(rng >> 8);
}

static int open_ctx(ctx_t* c, uint32_t server, int port) {
    memset(c, 0, sizeof *c);
    netconf_t n;
    if (sys_netconf(NETCONF_GET, &n) == 0) {
        strlcpy(c->domain, n.domain, sizeof c->domain);
        for (int i = 0; i < 2 && !server; i++)
            if (n.dns[i]) c->servers[c->n++] = n.dns[i];
    }
    if (server) c->servers[c->n++] = server;
    if (!c->n) return DNS_E_NOSERVER;
    c->port = port ? port : DNS_PORT;
    if ((c->fd = sys_socket(SOCK_DGRAM)) < 0) return c->fd;
    int p = sys_bind(c->fd, 0);
    if (p < 0) {
        sys_close(c->fd);
        return p;
    }
    rng ^= sys_ticks() * 2654435761u ^ (uint32_t)sys_getpid() << 16 ^ (uint32_t)p << 3;
    if (!rng) rng = 0x9E3779B9u;
    return 0;
}

static int rcode_error(int rcode) {
    switch (rcode) {
    case DNS_RCODE_SERVFAIL: return DNS_E_SERVFAIL;
    case DNS_RCODE_REFUSED: return DNS_E_REFUSED;
    default: return DNS_E_FORMERR;
    }
}

// One question, to each server in turn until one gives an answer that
// settles it: 0 with a filled in (an answer, or NXDOMAIN), or the
// error from the last server tried.
static int ask(ctx_t* c, const char* name, int type, dns_answer_t* a) {
    uint8_t q[DNS_QUERY_MAX], buf[UDP_MAX_SIZE];
    int err = DNS_E_TIMEOUT;
    for (int s = 0; s < c->n; s++) {
        uint16_t id = new_id();  // the same for both tries, so a late answer to the first still counts
        int qlen = dns_build_query(q, sizeof q, id, name, type);
        if (qlen < 0) return qlen;
        for (int t = 0; t < TRIES; t++) {
            udp_msg_t m = { c->servers[s], c->port, q, qlen, 0 };
            int r = sys_sendto(c->fd, &m);
            if (r == -ENETDOWN || r == -EADDRNOTAVAIL || r == -EINTR) return r;  // no server will do better
            if (r < 0) { err = r; break; }  // no way to this one (no gateway, say)
            uint32_t until = sys_ticks() + WAIT_TICKS;
            for (int32_t left; (left = (int32_t)(until - sys_ticks())) > 0;) {
                udp_msg_t in = { 0, 0, buf, sizeof buf, left * 10 };
                if ((r = sys_recvfrom(c->fd, &in)) == -ETIMEDOUT) break;
                if (r < 0) return r;
                if (in.ip != c->servers[s] || in.port != c->port) continue;
                r = dns_parse(buf, r, id, name, type, a);
                if (r == DNS_E_NOTOURS) continue;
                if (r == DNS_E_LOOP) return r;
                if (r == 0 && (a->rcode == 0 || a->rcode == DNS_RCODE_NXDOMAIN)) return 0;
                err = r ? r : rcode_error(a->rcode);
                goto next;
            }
        }
    next:;
    }
    return err;
}

// Asks about name and, if the answer stops at an alias, about where the
// alias leads, collecting the chain in r.
static int resolve(ctx_t* c, const char* name, int type, dns_result_t* r) {
    memset(r, 0, sizeof *r);
    strlcpy(r->name, name, sizeof r->name);
    int e = ask(c, name, type, &r->a);
    if (e) return e;
    dns_answer_t more;
    const dns_answer_t* last = &r->a;
    for (;;) {
        if (last->rcode == DNS_RCODE_NXDOMAIN) return DNS_E_NONAME;
        if (type == DNS_TYPE_A ? r->a.naddrs > 0 : r->a.ptr[0] != 0) return 0;
        if (last->truncated) return DNS_E_TRUNCATED;
        if (!last->nchain) return DNS_E_NODATA;
        if ((e = ask(c, r->a.chain[r->a.nchain - 1], type, &more))) return e;
        if (r->a.nchain + more.nchain > DNS_MAX_CHAIN) return DNS_E_LOOP;
        memcpy(r->a.chain[r->a.nchain], more.chain, sizeof more.chain[0] * (size_t)more.nchain);
        r->a.nchain += more.nchain;
        memcpy(r->a.addrs, more.addrs, sizeof more.addrs);
        r->a.naddrs = more.naddrs;
        memcpy(r->a.ptr, more.ptr, sizeof more.ptr);
        last = &more;
    }
}

// Digits and dots only: meant as an address, but not a good one.
static int numeric(const char* s) {
    for (; *s; s++)
        if ((*s < '0' || *s > '9') && *s != '.') return 0;
    return 1;
}

int dns_lookup(const char* name, uint32_t server, int port, dns_result_t* r) {
    memset(r, 0, sizeof *r);
    uint32_t ip;
    if (parse_ipv4(name, &ip) == 0 || dns_name_eq(name, "localhost")) {
        strlcpy(r->name, name, sizeof r->name);
        r->a.addrs[0] = parse_ipv4(name, &ip) == 0 ? ip : 0x7F000001u;
        r->a.naddrs = 1;
        return 1;
    }
    if (numeric(name) || !dns_name_ok(name)) return DNS_E_BADNAME;
    char bare[DNS_NAME_SIZE], full[DNS_NAME_SIZE];
    strlcpy(bare, name, sizeof bare);
    size_t len = strlen(bare);
    int absolute = bare[len - 1] == '.';
    if (absolute) bare[len - 1] = 0;
    ctx_t c;
    int e = open_ctx(&c, server, port);
    if (e) return e;
    e = DNS_E_NONAME;
    // "printer" is looked for as printer.<domain> first, as resolv.conf's
    // ndots:1 would; failing that, as itself.
    if (!absolute && !strchr(bare, '.') && c.domain[0]) {
        snprintf(full, sizeof full, "%s.%s", bare, c.domain);
        if (dns_name_ok(full)) e = resolve(&c, full, DNS_TYPE_A, r);
    }
    if (e == DNS_E_NONAME || e == DNS_E_NODATA) e = resolve(&c, bare, DNS_TYPE_A, r);
    sys_close(c.fd);
    return e ? e : r->a.naddrs;
}

int dns_reverse(uint32_t ip, uint32_t server, int port, dns_result_t* r) {
    char q[32];
    snprintf(q, sizeof q, "%u.%u.%u.%u.in-addr.arpa", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    if (ip >> 24 == 127) {
        memset(r, 0, sizeof *r);
        strlcpy(r->name, q, sizeof r->name);
        strlcpy(r->a.ptr, "localhost", sizeof r->a.ptr);
        return 0;
    }
    ctx_t c;
    int e = open_ctx(&c, server, port);
    if (e) return e;
    e = resolve(&c, q, DNS_TYPE_PTR, r);
    sys_close(c.fd);
    return e;
}

const char* dns_strerror(int err) {
    switch (err) {
    case DNS_E_NONAME: return "no such name";
    case DNS_E_NODATA: return "the name has no address";
    case DNS_E_SERVFAIL: return "the DNS server failed to find out";
    case DNS_E_REFUSED: return "the DNS server refused to answer";
    case DNS_E_FORMERR: return "the DNS server did not understand the question";
    case DNS_E_NOSERVER: return "no DNS server to ask (see net)";
    case DNS_E_BADNAME: return "not a valid name";
    case DNS_E_BADREPLY: return "the DNS server's answer made no sense";
    case DNS_E_TIMEOUT: return "no answer from the DNS server";
    case DNS_E_TRUNCATED: return "the answer is too big for UDP (no retry over TCP)";
    case DNS_E_LOOP: return "too many aliases";
    default: return strerror(err);
    }
}

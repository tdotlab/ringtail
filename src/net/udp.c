// UDP sockets. Each socket has a port once bound and a queue of the
// datagrams that came for it; the descriptors that refer to it count
// as references, and the last one to go frees it. A program waiting in
// SYS_RECVFROM sleeps on the socket itself, and the net task wakes it
// when a datagram is queued.
#include "kernel.h"
#include "arch.h"
#include "task.h"
#include "mm.h"
#include "fs.h"
#include "netpriv.h"

#define UDP_SOCKETS 32
#define UDP_QUEUE 16          // datagrams waiting per socket
#define EPHEMERAL_FIRST 49152 // free ports are picked from here to 65535
#define DHCP_CLIENT_PORT 68

typedef struct dgram {
    struct dgram* next;
    uint32_t ip;
    uint16_t port, len;
    uint8_t data[];
} dgram_t;

struct socket {
    int refs;
    uint16_t port;       // 0 until bound
    int queued;
    dgram_t *head, *tail;
};

static struct socket* socks[UDP_SOCKETS];
static struct { uint32_t in, out, closed, overflow; } us;

static struct socket* by_port(uint16_t port) {
    for (int i = 0; i < UDP_SOCKETS; i++)
        if (socks[i] && socks[i]->port == port) return socks[i];
    return 0;
}

struct socket* udp_socket_new(void) {
    struct socket* s = 0;
    mutex_lock(&net_lock);
    for (int i = 0; i < UDP_SOCKETS; i++) {
        if (socks[i]) continue;
        s = kmalloc(sizeof *s);
        if (s) {
            memset(s, 0, sizeof *s);
            s->refs = 1;
            socks[i] = s;
        }
        break;
    }
    mutex_unlock(&net_lock);
    return s;
}

void udp_socket_ref(struct socket* s) {
    mutex_lock(&net_lock);
    s->refs++;
    mutex_unlock(&net_lock);
}

void udp_socket_unref(struct socket* s) {
    mutex_lock(&net_lock);
    if (--s->refs == 0) {
        for (int i = 0; i < UDP_SOCKETS; i++)
            if (socks[i] == s) socks[i] = 0;
        while (s->head) {
            dgram_t* d = s->head;
            s->head = d->next;
            kfree(d);
        }
        kfree(s);
    }
    mutex_unlock(&net_lock);
}

int udp_bind(struct socket* s, int port) {
    if (port < 0 || port > 65535) return -EINVAL;
    mutex_lock(&net_lock);
    int r;
    if (s->port) r = -EINVAL;  // bound already
    else if (port == DHCP_CLIENT_PORT || (port && by_port((uint16_t)port))) r = -EADDRINUSE;
    else if (port) r = port;
    else {
        // A free port, starting somewhere unpredictable so that a
        // stranger cannot guess where the answer to a DNS query goes.
        uint32_t span = 65536 - EPHEMERAL_FIRST, start = net_random() % span;
        r = -EADDRINUSE;
        for (uint32_t i = 0; i < span; i++) {
            uint16_t p = (uint16_t)(EPHEMERAL_FIRST + (start + i) % span);
            if (!by_port(p)) { r = p; break; }
        }
    }
    if (r > 0) s->port = (uint16_t)r;
    mutex_unlock(&net_lock);
    return r;
}

void udp_header(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t len) {
    uint8_t* u = NET_PAYLOAD;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(UDP_HLEN + len));
    put16(u + 6, 0);
    uint8_t pseudo[12];
    put32(pseudo, src);
    put32(pseudo + 4, dst);
    put16(pseudo + 8, IP_PROTO_UDP);
    put16(pseudo + 10, (uint16_t)(UDP_HLEN + len));
    uint16_t c = csum_fold(csum_add(csum_add(0, pseudo, 12), u, UDP_HLEN + len));
    put16(u + 6, c ? c : 0xFFFF);  // 0 would mean "no checksum"
}

int udp_socket_port(struct socket* s) { return s->port; }

int udp_socket_events(struct socket* s) {
    mutex_lock(&net_lock);
    int ev = (s->head ? POLLIN : 0) | POLLOUT;
    mutex_unlock(&net_lock);
    return ev;
}

int udp_sendto(struct socket* s, uint32_t ip, int port, const uint8_t* kbuf, uint32_t len) {
    if (len > UDP_MAX_SIZE) return -EMSGSIZE;
    if (port <= 0 || port > 65535) return -EINVAL;
    if (ip != IP_BROADCAST && !ip_unicast(ip) && (ip >> 24) != 127) return -EINVAL;
    int r = net_can_send(ip);
    if (r) return r;
    if (!s->port && (r = udp_bind(s, 0)) < 0) return r;
    mutex_lock(&net_lock);
    uint8_t mac[ETH_ALEN];
    uint32_t hop;
    r = ip_route(ip, &hop);
    // Waiting for ARP lets go of the lock, and someone else may use
    // the transmit buffer meanwhile, so the datagram goes in after.
    if (!r && !ip_local(ip)) r = arp_resolve(hop, mac);
    if (!r) r = net_can_send(ip);  // the address may have gone while we waited
    if (!r) {
        memcpy(NET_PAYLOAD + UDP_HLEN, kbuf, len);
        uint32_t src = ip_source(ip);
        udp_header(src, ip, s->port, (uint16_t)port, len);
        r = ip_send(src, ip, mac, IP_PROTO_UDP, UDP_HLEN + len);
        if (!r) us.out++;
    }
    mutex_unlock(&net_lock);
    return r ? r : (int)len;
}

int udp_recvfrom(struct socket* s, uint32_t* ip, int* port, uint8_t* kbuf, uint32_t size, int timeout_ms) {
    mutex_lock(&net_lock);
    if (!s->port) { mutex_unlock(&net_lock); return -EINVAL; }  // nothing can come to it
    uint64_t ticks = timeout_ms > 0 ? ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000 : 0;
    uint32_t until = timer_ticks() + (uint32_t)ticks;
    int r;
    for (;;) {
        dgram_t* d = s->head;
        if (d) {
            s->head = d->next;
            if (!s->head) s->tail = 0;
            s->queued--;
            r = MIN(size, d->len);
            memcpy(kbuf, d->data, (uint32_t)r);
            *ip = d->ip;
            *port = d->port;
            kfree(d);
            break;
        }
        if (timeout_ms == 0) { r = -EAGAIN; break; }
        if (current->killed) { r = -EINTR; break; }
        if (timeout_ms > 0 && (int32_t)(timer_ticks() - until) >= 0) { r = -ETIMEDOUT; break; }
        mutex_unlock(&net_lock);
        uint32_t f = irq_save();
        if (!s->head && !current->killed) {
            if (timeout_ms > 0) task_block_timeout(s, until);
            else task_block(s);
        }
        irq_restore(f);
        mutex_lock(&net_lock);
    }
    mutex_unlock(&net_lock);
    return r;
}

int udp_input(const uint8_t* eth_src, const uint8_t* iph, uint32_t src, uint32_t dst, const uint8_t* p, uint32_t len) {
    if (len < UDP_HLEN) return -1;
    uint32_t ulen = get16(p + 4);
    if (ulen < UDP_HLEN || ulen > len) return -1;
    if (get16(p + 6)) {  // 0: the sender did not checksum it
        uint8_t pseudo[12];
        put32(pseudo, src);
        put32(pseudo + 4, dst);
        put16(pseudo + 8, IP_PROTO_UDP);
        put16(pseudo + 10, (uint16_t)ulen);
        if (csum_fold(csum_add(csum_add(0, pseudo, 12), p, ulen)) != 0) return -1;
    }
    uint16_t sport = get16(p), dport = get16(p + 2);
    if (dport == DHCP_CLIENT_PORT) {
        if (sport == 67) dhcp_input(p + UDP_HLEN, ulen - UDP_HLEN);
        return 0;
    }
    // While it has no address the machine takes UDP for any address,
    // but only so that DHCP works: nothing else is meant for it.
    if (!ip_local(dst) && !ip_broadcast(dst)) return -1;
    struct socket* s = by_port(dport);
    if (!s) {
        us.closed++;
        // Only a datagram meant for us alone earns an answer.
        if (ip_local(dst)) icmp_port_unreachable(eth_src, iph, get16(iph + 2));
        return 0;
    }
    uint32_t n = ulen - UDP_HLEN;
    dgram_t* d = s->queued < UDP_QUEUE ? kmalloc(sizeof *d + n) : 0;
    if (!d) { us.overflow++; return 0; }
    d->next = 0;
    d->ip = src;
    d->port = sport;
    d->len = (uint16_t)n;
    memcpy(d->data, p + UDP_HLEN, n);
    if (s->tail) s->tail->next = d;
    else s->head = d;
    s->tail = d;
    s->queued++;
    us.in++;
    task_wakeup(s);
    poll_notify();
    return 0;
}

void udp_format(sbuf_t* sb) {
    int open = 0;
    for (int i = 0; i < UDP_SOCKETS; i++) open += socks[i] != 0;
    sbprintf(sb, "UDP:      %d sockets; %u datagrams in, %u out, %u to closed ports, %u lost to full queues\n",
             open, us.in, us.out, us.closed, us.overflow);
}

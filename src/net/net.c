// The network stack's core: Ethernet in and out, ARP to find
// neighbours, IPv4 with no fragments and no options worth reading,
// ICMP echo both ways (answering pings, and SYS_PING), and loopback.
// UDP sockets are in udp.c, TCP in tcp.c (with tcpsock.c putting it
// in the kernel) and the DHCP client in dhcp.c.
//
// One kernel task, "net", takes frames off the card and packets off
// the loopback queue and handles them, and runs the timers of the DHCP
// client and of TCP. Everything that touches the stack's state (the address, the
// ARP table, the ping slots, the sockets, the counters and the transmit
// buffer) holds net_lock, so the net task and programs making network
// calls never run into each other. The card's interrupt only wakes the
// net task.
//
// The address comes from DHCP, or is set by hand with SYS_NETCONF.
// Packets to 127.x.x.x or to our own address never reach the card:
// they go on the loopback queue and come back in as if received, so
// they work with no card at all.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "mm.h"
#include "netpriv.h"
#include "random.h"
#include "tcp.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_UNREACHABLE  3
#define ICMP_ECHO_REQUEST 8

#define ARP_SLOTS 16
#define ARP_LIFETIME (10 * 60 * TIMER_HZ)  // forget a neighbour after 10 minutes
#define ARP_TRIES 3
#define ARP_RETRY TIMER_HZ  // as Linux: 3 asks, 1 s apart. Links can take that long to come up
#define PING_SLOTS 8
#define LO_QUEUE 64         // loopback packets waiting, at most
#define BOOT_WAIT (5 * TIMER_HZ)  // for DHCP before the shell starts

netdev_t* net_dev;
mutex_t net_lock = MUTEX_INIT;
uint8_t net_txbuf[ETH_FRAME_MAX];
ipconf_t net_cfg;
const uint8_t net_broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int rx_chan;              // the net task waits here for work
static int arp_chan;             // senders wait here for an ARP answer

static struct {
    uint32_t rx, tx, tx_failed;
    uint32_t dropped;            // malformed, bad checksum, or nothing here wants it
    uint32_t not_ours;           // for another address
    uint32_t fragments;          // IP fragments, which are not reassembled
    uint32_t pings_answered;
    uint32_t looped, lo_dropped; // through loopback, and lost for want of room
} st;

typedef struct {
    int used;
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    uint32_t when;
} arp_entry_t;
static arp_entry_t arp[ARP_SLOTS];
static uint32_t arp_asked_ip, arp_asked_at;  // arp_lookup's last request, to not repeat it

typedef struct {
    int used;
    uint32_t ip;
    uint16_t ident, seq;
    int size;
    uint32_t sent_us;
    volatile int result;  // 1 while waiting, then 0 or -errno
    int rtt_us, ttl;
} ping_slot_t;
static ping_slot_t pings[PING_SLOTS];

typedef struct lo_packet {
    struct lo_packet* next;
    uint32_t len;
    uint8_t data[];
} lo_packet_t;
static lo_packet_t *lo_head, *lo_tail;
static int lo_count;

// ---- Checksums and odds and ends ----

uint32_t csum_add(uint32_t sum, const uint8_t* p, uint32_t len) {
    for (; len > 1; p += 2, len -= 2) sum += get16(p);
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int mac_eq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, ETH_ALEN) == 0; }

// Ports, transaction ids, the secret behind TCP's sequence numbers:
// all things a stranger must not be able to guess, so they come from
// the kernel's generator (random.h).
uint32_t net_random(void) {
    uint32_t x;
    random_bytes(&x, sizeof x);
    return x;
}

void net_kick(void) { task_wakeup(&rx_chan); }

// ---- Ethernet ----

// Sends net_txbuf, whose payload the caller has filled in after the header.
static int eth_send(const uint8_t* dst, uint16_t type, uint32_t payload_len) {
    if (!net_dev) return -ENETDOWN;
    memcpy(net_txbuf, dst, ETH_ALEN);
    memcpy(net_txbuf + ETH_ALEN, net_dev->mac, ETH_ALEN);
    put16(net_txbuf + 12, type);
    uint32_t len = ETH_HLEN + payload_len;
    if (len < 60) {  // the minimum frame, before the CRC
        memset(net_txbuf + len, 0, 60 - len);
        len = 60;
    }
    int r = net_dev->send(net_dev, net_txbuf, len);
    if (r) st.tx_failed++;
    else st.tx++;
    return r ? -EIO : 0;
}

// ---- ARP ----

static arp_entry_t* arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp[i].used && arp[i].ip == ip) {
            if (timer_ticks() - arp[i].when > ARP_LIFETIME) { arp[i].used = 0; return 0; }
            return &arp[i];
        }
    return 0;
}

// Remember ip's MAC. Only an address that is already known is updated
// unless create is set: a host may learn its neighbours from requests
// aimed at it, but should not fill its table from everyone else's.
static void arp_learn(uint32_t ip, const uint8_t* mac, int create) {
    if (!ip || ip == IP_BROADCAST || (mac[0] & 1)) return;  // no address, or a group MAC
    arp_entry_t* e = arp_find(ip);
    if (!e) {
        if (!create) return;
        e = &arp[0];
        for (int i = 0; i < ARP_SLOTS; i++) {
            if (!arp[i].used) { e = &arp[i]; break; }
            if (arp[i].when < e->when) e = &arp[i];  // or the stalest
        }
    }
    e->used = 1;
    e->ip = ip;
    memcpy(e->mac, mac, ETH_ALEN);
    e->when = timer_ticks();
    task_wakeup(&arp_chan);
}

static void arp_send(uint16_t op, const uint8_t* eth_dst, const uint8_t* tha, uint32_t tpa) {
    uint8_t* a = net_txbuf + ETH_HLEN;
    put16(a, 1);            // Ethernet
    put16(a + 2, ETH_P_IP);
    a[4] = ETH_ALEN;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, net_dev->mac, ETH_ALEN);
    put32(a + 14, net_cfg.ip);
    memcpy(a + 18, tha, ETH_ALEN);
    put32(a + 24, tpa);
    eth_send(eth_dst, ETH_P_ARP, 28);
}

static void arp_request(uint32_t ip) {
    static const uint8_t zero[ETH_ALEN];
    arp_send(1, net_broadcast_mac, zero, ip);
}

static void arp_input(const uint8_t* p, uint32_t len) {
    if (len < 28 || get16(p) != 1 || get16(p + 2) != ETH_P_IP || p[4] != ETH_ALEN || p[5] != 4) {
        st.dropped++;
        return;
    }
    uint16_t op = get16(p + 6);
    const uint8_t* sha = p + 8;
    uint32_t spa = get32(p + 14), tpa = get32(p + 24);
    if (net_cfg.ip && spa == net_cfg.ip) { st.dropped++; return; }  // someone else claims our address; ignore them
    if (!net_cfg.ip || tpa != net_cfg.ip) {
        arp_learn(spa, sha, 0);
        st.not_ours++;
        return;
    }
    arp_learn(spa, sha, 1);
    if (op == 1) arp_send(2, sha, sha, spa);
}

int ip_broadcast(uint32_t ip) {
    return ip == IP_BROADCAST || (net_cfg.ip && (ip & ~net_cfg.mask) == ~net_cfg.mask &&
                                  (ip & net_cfg.mask) == (net_cfg.ip & net_cfg.mask));
}

int arp_lookup(uint32_t ip, uint8_t* mac) {
    if (ip_broadcast(ip)) { memcpy(mac, net_broadcast_mac, ETH_ALEN); return 0; }
    arp_entry_t* e = arp_find(ip);
    if (e) { memcpy(mac, e->mac, ETH_ALEN); return 0; }
    if (!net_dev || !net_cfg.ip) return -EHOSTUNREACH;
    if (ip != arp_asked_ip || timer_ticks() - arp_asked_at >= ARP_RETRY) {
        arp_request(ip);
        arp_asked_ip = ip;
        arp_asked_at = timer_ticks();
    }
    return -EAGAIN;
}

// Returns 0, or -EHOSTUNREACH when nobody answers, or -EINTR.
int arp_resolve(uint32_t ip, uint8_t* mac) {
    for (int tries = 0;; tries++) {
        if (tries == ARP_TRIES) {
            arp_entry_t* e = arp_find(ip);
            if (!e) return -EHOSTUNREACH;
            memcpy(mac, e->mac, ETH_ALEN);
            return 0;
        }
        int r = arp_lookup(ip, mac);  // asks again once a second has gone by
        if (r != -EAGAIN) return r;
        uint32_t until = timer_ticks() + ARP_RETRY;
        mutex_unlock(&net_lock);
        uint32_t f = irq_save();
        while (!arp_find(ip) && !current->killed && (int32_t)(timer_ticks() - until) < 0)
            task_block_timeout(&arp_chan, until);
        irq_restore(f);
        mutex_lock(&net_lock);
        if (current->killed) return -EINTR;
    }
}

// ---- Addresses and routes ----

int ip_local(uint32_t ip) { return (ip >> 24) == 127 || (net_cfg.ip && ip == net_cfg.ip); }

uint32_t ip_source(uint32_t dst) { return (dst >> 24) == 127 ? dst : net_cfg.ip; }

int ip_route(uint32_t dst, uint32_t* hop) {
    *hop = dst;
    if (ip_local(dst) || dst == IP_BROADCAST) return 0;
    if (net_cfg.ip && (dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) return 0;
    if (!net_cfg.gw) return -ENETUNREACH;
    *hop = net_cfg.gw;
    return 0;
}

void net_set_config(const ipconf_t* c) {
    if (net_cfg.ip && c->ip != net_cfg.ip) {
        tcp_addr_lost(net_cfg.ip);
        tcp_flush_wakeups();
    }
    if (c->ip != net_cfg.ip || c->mask != net_cfg.mask) memset(arp, 0, sizeof arp);
    net_cfg = *c;
}

// ---- Loopback ----

static int lo_send(const uint8_t* packet, uint32_t len) {
    lo_packet_t* q = lo_count < LO_QUEUE ? kmalloc(sizeof *q + len) : 0;
    if (!q) { st.lo_dropped++; return -ENOMEM; }
    q->next = 0;
    q->len = len;
    memcpy(q->data, packet, len);
    if (lo_tail) lo_tail->next = q;
    else lo_head = q;
    lo_tail = q;
    lo_count++;
    st.looped++;
    task_wakeup(&rx_chan);
    return 0;
}

// ---- IPv4 and ICMP ----

static uint16_t ip_id;

int ip_send(uint32_t src, uint32_t dst, const uint8_t* mac, uint8_t proto, uint32_t len) {
    uint8_t* h = net_txbuf + ETH_HLEN;
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, IP_HLEN + len);
    put16(h + 4, ip_id++);
    put16(h + 6, 0x4000);  // don't fragment
    h[8] = 64;
    h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, src ? src : ip_source(dst));
    put32(h + 16, dst);
    put16(h + 10, inet_csum(h, IP_HLEN));
    if (ip_local(dst)) return lo_send(h, IP_HLEN + len);
    return eth_send(mac, ETH_P_IP, IP_HLEN + len);
}

static uint8_t ping_byte(int i, uint16_t seq) { return (uint8_t)(i * 7 + seq); }

static void ping_reply(uint32_t src, uint16_t ident, uint16_t seq, const uint8_t* data, uint32_t len, int ttl) {
    for (int i = 0; i < PING_SLOTS; i++) {
        ping_slot_t* s = &pings[i];
        if (!s->used || s->result != 1 || s->ip != src || s->ident != ident || s->seq != seq) continue;
        int intact = len == (uint32_t)s->size;
        for (uint32_t j = 0; intact && j < len; j++) intact = data[j] == ping_byte(j, seq);
        s->rtt_us = (int)(timer_micros() - s->sent_us);
        s->ttl = ttl;
        s->result = intact ? 0 : -EIO;
        task_wakeup(s);
        return;
    }
    st.dropped++;  // late, or not ours
}

// A router's "destination unreachable" quotes the start of the packet
// it could not deliver: if that was one of our echo requests, its
// ping fails at once instead of timing out.
static void ping_unreachable(const uint8_t* q, uint32_t len, int code) {
    if (len < IP_HLEN + 8 || (q[0] >> 4) != 4) return;
    uint32_t ihl = (q[0] & 15) * 4u;
    if (ihl < IP_HLEN || len < ihl + 8 || q[9] != IP_PROTO_ICMP || q[ihl] != ICMP_ECHO_REQUEST) return;
    uint32_t dst = get32(q + 16);
    uint16_t ident = get16(q + ihl + 4), seq = get16(q + ihl + 6);
    for (int i = 0; i < PING_SLOTS; i++) {
        ping_slot_t* s = &pings[i];
        if (s->used && s->result == 1 && s->ip == dst && s->ident == ident && s->seq == seq) {
            s->result = code == 0 ? -ENETUNREACH : -EHOSTUNREACH;
            task_wakeup(s);
        }
    }
}

// The same for a TCP segment: an attempt to connect fails at once.
// "Fragmentation needed" is no failure (we never send more than fits).
static void tcp_unreachable(const uint8_t* q, uint32_t len, int code) {
    if (len < IP_HLEN + 8 || (q[0] >> 4) != 4) return;
    uint32_t ihl = (q[0] & 15) * 4u;
    if (ihl < IP_HLEN || len < ihl + 8 || q[9] != IP_PROTO_TCP || code == 4) return;
    int err = code == 0 ? -ENETUNREACH : code == 2 || code == 3 ? -ECONNREFUSED : -EHOSTUNREACH;
    tcp_icmp_error(get32(q + 12), get32(q + 16), q + ihl, err);
    tcp_flush_wakeups();
}

// Quotes the IP header and the first 8 bytes after it, as RFC 792 asks.
void icmp_port_unreachable(const uint8_t* eth_src, const uint8_t* iph, uint32_t len) {
    uint32_t quote = MIN(len, (iph[0] & 15) * 4u + 8);
    uint8_t* m = NET_PAYLOAD;
    m[0] = ICMP_UNREACHABLE;
    m[1] = 3;  // port unreachable
    memset(m + 2, 0, 6);
    memcpy(m + 8, iph, quote);
    put16(m + 2, inet_csum(m, 8 + quote));
    ip_send(get32(iph + 16), get32(iph + 12), eth_src, IP_PROTO_ICMP, 8 + quote);
}

static void icmp_input(const uint8_t* eth_src, uint32_t src, uint32_t dst, int ttl, const uint8_t* p, uint32_t len) {
    if (len < 8 || inet_csum(p, len) != 0) { st.dropped++; return; }
    switch (p[0]) {
    case ICMP_ECHO_REQUEST:
        // Not for broadcasts: one ping must not make every host answer.
        if (!ip_local(dst) || p[1] != 0) { st.dropped++; return; }
        // Straight back to the frame's sender, which is the host itself
        // or the router it came through, so no ARP lookup is needed.
        memmove(NET_PAYLOAD, p, len);
        NET_PAYLOAD[0] = ICMP_ECHO_REPLY;
        put16(NET_PAYLOAD + 2, 0);
        put16(NET_PAYLOAD + 2, inet_csum(NET_PAYLOAD, len));
        if (ip_send(dst, src, eth_src, IP_PROTO_ICMP, len) == 0) st.pings_answered++;
        return;
    case ICMP_ECHO_REPLY:
        ping_reply(src, get16(p + 4), get16(p + 6), p + 8, len - 8, ttl);
        return;
    case ICMP_UNREACHABLE:
        ping_unreachable(p + 8, len - 8, p[1]);
        tcp_unreachable(p + 8, len - 8, p[1]);
        return;
    }
    st.dropped++;
}

// eth is the frame the packet came in, or 0 for loopback.
static void ip_input(const uint8_t* eth, const uint8_t* p, uint32_t len) {
    if (len < IP_HLEN || (p[0] >> 4) != 4) { st.dropped++; return; }
    uint32_t ihl = (p[0] & 15) * 4u, total = get16(p + 2);
    // The frame may carry padding after the packet, but never less
    // than the packet says it holds.
    if (ihl < IP_HLEN || total < ihl || total > len || inet_csum(p, ihl) != 0) { st.dropped++; return; }
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    if (eth) {
        // Nothing from outside may claim to be loopback, or us, or a group.
        if (!ip_unicast(src) || ip_local(src) || (dst >> 24) == 127) { st.dropped++; return; }
        int ours = (net_cfg.ip && dst == net_cfg.ip) || ip_broadcast(dst);
        // Before it has an address, the DHCP client must take what a
        // server sends to the address it is offering.
        int dhcp = !net_cfg.ip && p[9] == IP_PROTO_UDP && total >= ihl + 4 && get16(p + ihl + 2) == 68;
        if (!ours && !dhcp) { st.not_ours++; return; }
    }
    if (get16(p + 6) & 0x3FFF) { st.fragments++; return; }  // more fragments, or an offset
    const uint8_t* eth_src = eth ? eth + ETH_ALEN : 0;
    if (p[9] == IP_PROTO_ICMP) icmp_input(eth_src, src, dst, p[8], p + ihl, total - ihl);
    else if (p[9] == IP_PROTO_UDP) {
        if (udp_input(eth_src, p, src, dst, p + ihl, total - ihl)) st.dropped++;
    } else if (p[9] == IP_PROTO_TCP && ip_local(dst)) {  // never a broadcast's
        tcp_input(src, dst, p + ihl, total - ihl, eth_src);
        tcp_flush_wakeups();
    } else st.dropped++;
}

static void eth_input(const uint8_t* f, uint32_t len) {
    st.rx++;
    if (len < ETH_HLEN) { st.dropped++; return; }
    if (!mac_eq(f, net_dev->mac) && !mac_eq(f, net_broadcast_mac)) { st.not_ours++; return; }
    if (f[ETH_ALEN] & 1) { st.dropped++; return; }  // a group address can never be a sender
    switch (get16(f + 12)) {
    case ETH_P_ARP: arp_input(f + ETH_HLEN, len - ETH_HLEN); return;
    case ETH_P_IP: ip_input(f, f + ETH_HLEN, len - ETH_HLEN); return;
    }
    st.dropped++;
}

// ---- The net task ----

void net_rx_notify(void) { task_wakeup(&rx_chan); }

static void net_main(void* arg) {
    (void)arg;
    static uint8_t frame[ETH_FRAME_MAX + 4];
    for (;;) {
        mutex_lock(&net_lock);
        lo_packet_t* q = lo_head;
        if (q) {
            lo_head = q->next;
            if (!lo_head) lo_tail = 0;
            lo_count--;
            ip_input(0, q->data, q->len);
        }
        // The timers run after every loopback packet too, so a busy
        // connection over loopback cannot hold them off.
        uint64_t now = net_now(), next = dhcp_poll(now);
        uint32_t tcp_ms = tcp_timers();
        tcp_flush_wakeups();
        if (tcp_ms != ~0u) next = MIN(next, now + ((uint64_t)tcp_ms * TIMER_HZ + 999) / 1000);
        mutex_unlock(&net_lock);
        if (q) {
            kfree(q);
            continue;
        }
        if (net_dev) {
            int n = net_dev->recv(net_dev, frame, sizeof frame);
            if (n > 0) {
                mutex_lock(&net_lock);
                eth_input(frame, (uint32_t)n);
                mutex_unlock(&net_lock);
                continue;
            }
        }
        // Without an interrupt the card is polled every tick; with one,
        // a lost interrupt still costs at most a second.
        uint32_t wait = net_dev && net_dev->irq < 0 ? 1 : TIMER_HZ;
        if (next - now < wait) wait = next > now ? (uint32_t)(next - now) : 0;
        uint32_t f = irq_save();
        if (wait && !lo_head && !(net_dev && net_dev->rx_ready(net_dev)))
            task_block_timeout(&rx_chan, timer_ticks() + wait);
        irq_restore(f);
    }
}

static void print_ip(sbuf_t* sb, uint32_t ip) {
    sbprintf(sb, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static void print_mac(sbuf_t* sb, const uint8_t* m) {
    sbprintf(sb, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int mask_bits(uint32_t m) {
    int bits = 0;
    for (; m & 0x80000000u; m <<= 1) bits++;
    return bits;
}

void net_init(void) {
    net_dev = e1000_probe();
    if (task_create_kernel("net", net_main, 0) < 0) {
        kprintf_color(COLOR_ERROR, "Network: cannot start the net task\n");
        net_dev = 0;
        return;
    }
    if (!net_dev) {
        int found = 0;
        for (int i = 0; i < pci_count(); i++) found += pci_get(i)->class_code == 0x02;
        if (found) kprintf_color(COLOR_DIM, "Network: no driver for this card (see `pci`)\n");
        else kprintf_color(COLOR_DIM, "Network: no card\n");
        return;
    }
    char line[64];
    sbuf_t sb = { line, sizeof line, 0 };
    print_mac(&sb, net_dev->mac);
    kprintf("Network: %s %s %s\n", net_dev->name, net_dev->model, line);
    mutex_lock(&net_lock);
    dhcp_start();
    mutex_unlock(&net_lock);
}

// From init, before the shell: give DHCP a few seconds, so that the
// first command typed finds an address, and say what came of it.
void net_boot_wait(void) {
    if (!net_dev) return;
    uint32_t until = timer_ticks() + BOOT_WAIT;
    while (!net_cfg.ip && (int32_t)(timer_ticks() - until) < 0) task_sleep_ticks(5);
    char line[128];
    sbuf_t sb = { line, sizeof line, 0 };
    mutex_lock(&net_lock);
    if (net_cfg.ip) {
        print_ip(&sb, net_cfg.ip);
        sbprintf(&sb, "/%d", mask_bits(net_cfg.mask));
        if (net_cfg.gw) { sbprintf(&sb, ", gateway "); print_ip(&sb, net_cfg.gw); }
        if (net_cfg.dns[0]) { sbprintf(&sb, ", DNS "); print_ip(&sb, net_cfg.dns[0]); }
        sbprintf(&sb, " (DHCP)");
    }
    mutex_unlock(&net_lock);
    if (sb.len) kprintf("Network: %s\n", line);
    else kprintf_color(COLOR_DIM, "Network: no answer from DHCP yet; still asking\n");
}

// ---- SYS_PING ----

int net_can_send(uint32_t ip) {
    if (ip_local(ip)) return 0;
    if (!net_dev || !net_dev->link_up(net_dev)) return -ENETDOWN;
    if (!net_cfg.ip && ip != IP_BROADCAST) return -EADDRNOTAVAIL;
    return 0;
}

int net_ping(ping_t* p) {
    if (p->size < 0 || p->size > PING_MAX_SIZE || p->timeout_ms <= 0 || p->timeout_ms > 10000) return -EINVAL;
    if (!p->ip || p->ip == IP_BROADCAST || (p->ip >> 28) >= 14) return -EINVAL;  // multicast and up
    int r = net_can_send(p->ip);
    if (r) return r;

    mutex_lock(&net_lock);
    uint8_t mac[ETH_ALEN];
    uint32_t hop;
    r = ip_route(p->ip, &hop);
    if (!r && !ip_local(p->ip)) r = arp_resolve(hop, mac);
    ping_slot_t* s = 0;
    for (int i = 0; !r && i < PING_SLOTS; i++)
        if (!pings[i].used) { s = &pings[i]; break; }
    if (!r && !s) r = -EAGAIN;
    if (r) { mutex_unlock(&net_lock); return r; }

    static uint16_t next_ident = 0x5254;
    s->used = 1;
    s->ip = p->ip;
    s->ident = next_ident++;
    s->seq = (uint16_t)p->seq;
    s->size = p->size;
    s->result = 1;
    uint8_t* icmp = NET_PAYLOAD;
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, s->ident);
    put16(icmp + 6, s->seq);
    for (int i = 0; i < p->size; i++) icmp[8 + i] = ping_byte(i, s->seq);
    put16(icmp + 2, inet_csum(icmp, 8 + p->size));
    s->sent_us = timer_micros();
    // The wait for the reply starts now, not before the ARP lookup.
    uint32_t until = timer_ticks() + ((uint32_t)p->timeout_ms * TIMER_HZ + 999) / 1000;
    if (ip_send(0, p->ip, mac, IP_PROTO_ICMP, 8 + p->size)) s->result = -EIO;
    mutex_unlock(&net_lock);

    uint32_t f = irq_save();
    while (s->result == 1 && !current->killed && (int32_t)(timer_ticks() - until) < 0)
        task_block_timeout(s, until);
    irq_restore(f);

    mutex_lock(&net_lock);
    r = s->result == 1 ? (current->killed ? -EINTR : -ETIMEDOUT) : s->result;
    if (r == 0) { p->rtt_us = s->rtt_us; p->ttl = s->ttl; }
    s->used = 0;
    mutex_unlock(&net_lock);
    return r;
}

// ---- SYS_NETCONF ----

static int domain_ok(const char* d) {
    for (int i = 0; d[i]; i++) {
        char c = d[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == '_')) return 0;
    }
    return 1;
}

int net_conf(int op, netconf_t* c) {
    if (op == NETCONF_GET) {
        memset(c, 0, sizeof *c);
        mutex_lock(&net_lock);
        c->ip = net_cfg.ip;
        c->mask = net_cfg.mask;
        c->gw = net_cfg.gw;
        c->dns[0] = net_cfg.dns[0];
        c->dns[1] = net_cfg.dns[1];
        strlcpy(c->domain, net_cfg.domain, sizeof c->domain);
        dhcp_conf(c);
        mutex_unlock(&net_lock);
        return 0;
    }
    if (op != NETCONF_STATIC && op != NETCONF_DHCP) return -EINVAL;
    if (!net_dev) return -ENETDOWN;
    if (op == NETCONF_DHCP) {
        mutex_lock(&net_lock);
        dhcp_start();
        mutex_unlock(&net_lock);
        net_kick();
        return 0;
    }
    uint32_t ip = c->ip, mask = c->mask, inv = ~mask;
    if (!ip_unicast(ip) || (inv & (inv + 1)) || mask_bits(mask) < 1 || mask_bits(mask) > 30) return -EINVAL;
    if ((ip & inv) == 0 || (ip & inv) == inv) return -EINVAL;  // the network's own address, or its broadcast
    if (c->gw && (!ip_unicast(c->gw) || c->gw == ip || (c->gw & mask) != (ip & mask) ||
                  (c->gw & inv) == 0 || (c->gw & inv) == inv)) return -EINVAL;
    for (int i = 0; i < 2; i++)
        if (c->dns[i] && !ip_unicast(c->dns[i])) return -EINVAL;
    c->domain[sizeof c->domain - 1] = 0;
    if (!domain_ok(c->domain)) return -EINVAL;
    ipconf_t n = { ip, mask, c->gw, { c->dns[0], c->dns[1] }, "" };
    strlcpy(n.domain, c->domain, sizeof n.domain);
    mutex_lock(&net_lock);
    dhcp_stop();
    net_set_config(&n);
    mutex_unlock(&net_lock);
    return 0;
}

// ---- KINFO_NET ----

void net_format(sbuf_t* sb) {
    if (!net_dev) {
        char what[48];
        int found = 0;
        for (int i = 0; i < pci_count(); i++) {
            const pci_dev_t* d = pci_get(i);
            if (d->class_code != 0x02) continue;
            sbprintf(sb, "No driver for %04x:%04x (%s) at %02x:%02x.%u\n", d->vendor, d->device,
                     pci_describe(d, what, sizeof what), d->bus, d->dev, d->fn);
            found++;
        }
        if (!found) sbprintf(sb, "No network card.\n");
        sbprintf(sb, "Loopback: 127.0.0.1, %u packets\n", st.looped);
        mutex_lock(&net_lock);
        udp_format(sb);
        tcp_format(sb);
        mutex_unlock(&net_lock);
        return;
    }
    mutex_lock(&net_lock);
    netdev_t* dev = net_dev;
    sbprintf(sb, "Card:     %s %s (%04x:%04x) at %s, irq ", dev->name, dev->model, dev->vendor, dev->device, dev->where);
    if (dev->irq < 0) sbprintf(sb, "none (polled)");
    else sbprintf(sb, "%d (%u interrupts)", dev->irq, dev->interrupts);
    sbprintf(sb, "\nMAC:      ");
    print_mac(sb, dev->mac);
    sbprintf(sb, ", link %s\nAddress:  ", dev->link_up(dev) ? "up" : "down");
    if (net_cfg.ip) {
        print_ip(sb, net_cfg.ip);
        sbprintf(sb, "/%d, ", mask_bits(net_cfg.mask));
        if (net_cfg.gw) { sbprintf(sb, "gateway "); print_ip(sb, net_cfg.gw); }
        else sbprintf(sb, "no gateway");
    } else sbprintf(sb, "none yet");
    sbprintf(sb, "\n");
    dhcp_format(sb);
    sbprintf(sb, "DNS:      ");
    if (!net_cfg.dns[0]) sbprintf(sb, "none");
    for (int i = 0; i < 2 && net_cfg.dns[i]; i++) {
        if (i) sbprintf(sb, ", ");
        print_ip(sb, net_cfg.dns[i]);
    }
    if (net_cfg.domain[0]) sbprintf(sb, ", domain %s", net_cfg.domain);
    sbprintf(sb, "\nFrames:   %u in, %u out", st.rx, st.tx);
    if (st.tx_failed) sbprintf(sb, " (%u failed)", st.tx_failed);
    sbprintf(sb, "; %u dropped, %u for others, %u fragments, %u bad at the card\n",
             st.dropped, st.not_ours, st.fragments, dev->rx_errors);
    sbprintf(sb, "Pings:    %u answered; %u packets through loopback\n", st.pings_answered, st.looped);
    udp_format(sb);
    tcp_format(sb);
    int n = 0;
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (!arp[i].used || timer_ticks() - arp[i].when > ARP_LIFETIME) continue;
        sbprintf(sb, n++ ? "          " : "ARP:      ");
        print_ip(sb, arp[i].ip);
        sbprintf(sb, " is ");
        print_mac(sb, arp[i].mac);
        sbprintf(sb, "\n");
    }
    mutex_unlock(&net_lock);
}

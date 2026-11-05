// The network stack: Ethernet in and out, ARP to find neighbours,
// IPv4 with no fragments and no options worth reading, and ICMP echo
// both ways (answering pings, and SYS_PING).
//
// One kernel task, "net", takes frames off the card and handles them.
// Everything that touches the stack's state (the ARP table, the ping
// slots, the counters and the transmit buffer) holds net_lock, so the
// net task and a program calling SYS_PING never run into each other.
// The card's interrupt only wakes the net task.
//
// The address is fixed for now: 10.0.2.15/24 with the gateway at
// 10.0.2.2, which is what QEMU's user networking expects.
#include "kernel.h"
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "task.h"
#include "sync.h"
#include "net.h"

#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806
#define IP_HLEN   20
#define IP_PROTO_ICMP 1
#define ICMP_ECHO_REPLY   0
#define ICMP_UNREACHABLE  3
#define ICMP_ECHO_REQUEST 8

#define ARP_SLOTS 16
#define ARP_LIFETIME (10 * 60 * TIMER_HZ)  // forget a neighbour after 10 minutes
#define ARP_TRIES 3
#define ARP_RETRY TIMER_HZ  // as Linux: 3 asks, 1 s apart. Links can take that long to come up
#define PING_SLOTS 8

static netdev_t* dev;
static mutex_t net_lock = MUTEX_INIT;
static int rx_chan;              // the net task waits here for frames
static int arp_chan;             // pings wait here for an ARP answer

static uint32_t my_ip = IPV4(10, 0, 2, 15);
static uint32_t my_mask = IPV4(255, 255, 255, 0);
static uint32_t my_gw = IPV4(10, 0, 2, 2);
static const uint8_t broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct {
    uint32_t rx, tx, tx_failed;
    uint32_t dropped;            // malformed, bad checksum, or nothing here wants it
    uint32_t not_ours;           // for another address
    uint32_t fragments;          // IP fragments, which are not reassembled
    uint32_t pings_answered;
} st;

typedef struct {
    int used;
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    uint32_t when;
} arp_entry_t;
static arp_entry_t arp[ARP_SLOTS];

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

// The frame being built. Only touched with net_lock held.
static uint8_t txbuf[ETH_FRAME_MAX];

// ---- Byte order and checksums ----

static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// The Internet checksum (RFC 1071). Over data that already holds its
// checksum, a correct one gives 0.
static uint16_t inet_csum(const uint8_t* p, uint32_t len) {
    uint32_t sum = 0;
    for (; len > 1; p += 2, len -= 2) sum += get16(p);
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int mac_eq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, ETH_ALEN) == 0; }

// ---- Ethernet ----

// Sends txbuf, whose payload the caller has filled in after the header.
static int eth_send(const uint8_t* dst, uint16_t type, uint32_t payload_len) {
    memcpy(txbuf, dst, ETH_ALEN);
    memcpy(txbuf + ETH_ALEN, dev->mac, ETH_ALEN);
    put16(txbuf + 12, type);
    uint32_t len = ETH_HLEN + payload_len;
    if (len < 60) {  // the minimum frame, before the CRC
        memset(txbuf + len, 0, 60 - len);
        len = 60;
    }
    int r = dev->send(dev, txbuf, len);
    if (r) st.tx_failed++;
    else st.tx++;
    return r;
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
    if (!ip || ip == 0xFFFFFFFFu || (mac[0] & 1)) return;  // no address, or a group MAC
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
    uint8_t* a = txbuf + ETH_HLEN;
    put16(a, 1);            // Ethernet
    put16(a + 2, ETH_P_IP);
    a[4] = ETH_ALEN;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, dev->mac, ETH_ALEN);
    put32(a + 14, my_ip);
    memcpy(a + 18, tha, ETH_ALEN);
    put32(a + 24, tpa);
    eth_send(eth_dst, ETH_P_ARP, 28);
}

static void arp_input(const uint8_t* p, uint32_t len) {
    if (len < 28 || get16(p) != 1 || get16(p + 2) != ETH_P_IP || p[4] != ETH_ALEN || p[5] != 4) {
        st.dropped++;
        return;
    }
    uint16_t op = get16(p + 6);
    const uint8_t* sha = p + 8;
    uint32_t spa = get32(p + 14), tpa = get32(p + 24);
    if (spa == my_ip) { st.dropped++; return; }  // someone else claims our address; ignore them
    if (tpa != my_ip) {
        arp_learn(spa, sha, 0);
        st.not_ours++;
        return;
    }
    arp_learn(spa, sha, 1);
    if (op == 1) arp_send(2, sha, sha, spa);
}

// The MAC to send to for ip, asking for it if need be. Called with
// net_lock held, which is dropped while waiting. Returns 0, or
// -EHOSTUNREACH when nobody answers, or -EINTR.
static int arp_resolve(uint32_t ip, uint8_t* mac) {
    if ((ip & ~my_mask) == ~my_mask || ip == 0xFFFFFFFFu) {  // a broadcast address
        memcpy(mac, broadcast_mac, ETH_ALEN);
        return 0;
    }
    for (int tries = 0;; tries++) {
        arp_entry_t* e = arp_find(ip);
        if (e) { memcpy(mac, e->mac, ETH_ALEN); return 0; }
        if (tries == ARP_TRIES) return -EHOSTUNREACH;
        static const uint8_t zero[ETH_ALEN];
        arp_send(1, broadcast_mac, zero, ip);
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

// ---- IPv4 and ICMP ----

static uint16_t ip_id;

// Fill in an IPv4 header at txbuf's payload for len bytes of data
// that follow it.
static void ip_header(uint32_t dst, uint8_t proto, uint32_t len) {
    uint8_t* h = txbuf + ETH_HLEN;
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, IP_HLEN + len);
    put16(h + 4, ip_id++);
    put16(h + 6, 0x4000);  // don't fragment
    h[8] = 64;
    h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, my_ip);
    put32(h + 16, dst);
    put16(h + 10, inet_csum(h, IP_HLEN));
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

static void icmp_input(const uint8_t* eth_src, uint32_t src, uint32_t dst, int ttl, const uint8_t* p, uint32_t len) {
    if (len < 8 || inet_csum(p, len) != 0) { st.dropped++; return; }
    switch (p[0]) {
    case ICMP_ECHO_REQUEST:
        // Not for broadcasts: one ping must not make every host answer.
        if (dst != my_ip || p[1] != 0) { st.dropped++; return; }
        // Straight back to the frame's sender, which is the host itself
        // or the router it came through, so no ARP lookup is needed.
        memcpy(txbuf + ETH_HLEN + IP_HLEN, p, len);
        txbuf[ETH_HLEN + IP_HLEN] = ICMP_ECHO_REPLY;
        put16(txbuf + ETH_HLEN + IP_HLEN + 2, 0);
        put16(txbuf + ETH_HLEN + IP_HLEN + 2, inet_csum(txbuf + ETH_HLEN + IP_HLEN, len));
        ip_header(src, IP_PROTO_ICMP, len);
        if (eth_send(eth_src, ETH_P_IP, IP_HLEN + len) == 0) st.pings_answered++;
        return;
    case ICMP_ECHO_REPLY:
        ping_reply(src, get16(p + 4), get16(p + 6), p + 8, len - 8, ttl);
        return;
    case ICMP_UNREACHABLE:
        ping_unreachable(p + 8, len - 8, p[1]);
        return;
    }
    st.dropped++;
}

static void ip_input(const uint8_t* eth, const uint8_t* p, uint32_t len) {
    if (len < IP_HLEN || (p[0] >> 4) != 4) { st.dropped++; return; }
    uint32_t ihl = (p[0] & 15) * 4u, total = get16(p + 2);
    // The frame may carry padding after the packet, but never less
    // than the packet says it holds.
    if (ihl < IP_HLEN || total < ihl || total > len || inet_csum(p, ihl) != 0) { st.dropped++; return; }
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    if (dst != my_ip && dst != 0xFFFFFFFFu && dst != (my_ip | ~my_mask)) { st.not_ours++; return; }
    if (get16(p + 6) & 0x3FFF) { st.fragments++; return; }  // more fragments, or an offset
    if (p[9] == IP_PROTO_ICMP) icmp_input(eth + ETH_ALEN, src, dst, p[8], p + ihl, total - ihl);
    else st.dropped++;
}

static void eth_input(const uint8_t* f, uint32_t len) {
    st.rx++;
    if (len < ETH_HLEN) { st.dropped++; return; }
    if (!mac_eq(f, dev->mac) && !mac_eq(f, broadcast_mac)) { st.not_ours++; return; }
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
        int n = dev->recv(dev, frame, sizeof frame);
        if (n > 0) {
            mutex_lock(&net_lock);
            eth_input(frame, (uint32_t)n);
            mutex_unlock(&net_lock);
            continue;
        }
        // Without an interrupt the card is polled every tick; with one,
        // a lost interrupt still costs at most a second.
        uint32_t f = irq_save();
        if (!dev->rx_ready(dev)) task_block_timeout(&rx_chan, timer_ticks() + (dev->irq < 0 ? 1 : TIMER_HZ));
        irq_restore(f);
    }
}

static void print_ip(sbuf_t* sb, uint32_t ip) {
    sbprintf(sb, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static void print_mac(sbuf_t* sb, const uint8_t* m) {
    sbprintf(sb, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void net_init(void) {
    dev = e1000_probe();
    char line[96];
    sbuf_t sb = { line, sizeof line, 0 };
    if (!dev) {
        int found = 0;
        for (int i = 0; i < pci_count(); i++) found += pci_get(i)->class_code == 0x02;
        if (found) kprintf_color(COLOR_DIM, "Network: no driver for this card (see `pci`)\n");
        else kprintf_color(COLOR_DIM, "Network: no card\n");
        return;
    }
    print_mac(&sb, dev->mac);
    sbprintf(&sb, ", ");
    print_ip(&sb, my_ip);
    kprintf("Network: %s %s %s\n", dev->name, dev->model, line);
    if (task_create_kernel("net", net_main, 0) < 0) {
        kprintf_color(COLOR_ERROR, "Network: cannot start the net task\n");
        dev = 0;
    }
}

// ---- SYS_PING ----

int net_ping(ping_t* p) {
    if (p->size < 0 || p->size > PING_MAX_SIZE || p->timeout_ms <= 0 || p->timeout_ms > 10000) return -EINVAL;
    if (!p->ip || p->ip == 0xFFFFFFFFu || (p->ip >> 28) >= 14) return -EINVAL;  // multicast and up
    if (!dev || !dev->link_up(dev)) return -ENETDOWN;
    if (p->ip == my_ip || (p->ip >> 24) == 127) return -EHOSTUNREACH;  // no loopback yet
    uint32_t next_hop = (p->ip & my_mask) == (my_ip & my_mask) ? p->ip : my_gw;

    mutex_lock(&net_lock);
    uint8_t mac[ETH_ALEN];
    int r = arp_resolve(next_hop, mac);
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
    uint8_t* icmp = txbuf + ETH_HLEN + IP_HLEN;
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, s->ident);
    put16(icmp + 6, s->seq);
    for (int i = 0; i < p->size; i++) icmp[8 + i] = ping_byte(i, s->seq);
    put16(icmp + 2, inet_csum(icmp, 8 + p->size));
    ip_header(p->ip, IP_PROTO_ICMP, 8 + p->size);
    s->sent_us = timer_micros();
    // The wait for the reply starts now, not before the ARP lookup.
    uint32_t until = timer_ticks() + ((uint32_t)p->timeout_ms * TIMER_HZ + 999) / 1000;
    if (eth_send(mac, ETH_P_IP, IP_HLEN + 8 + p->size)) s->result = -EIO;
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

// ---- KINFO_NET ----

void net_format(sbuf_t* sb) {
    if (!dev) {
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
        return;
    }
    mutex_lock(&net_lock);
    sbprintf(sb, "Card:     %s %s (%04x:%04x) at %s, irq ", dev->name, dev->model, dev->vendor, dev->device, dev->where);
    if (dev->irq < 0) sbprintf(sb, "none (polled)");
    else sbprintf(sb, "%d (%u interrupts)", dev->irq, dev->interrupts);
    sbprintf(sb, "\nMAC:      ");
    print_mac(sb, dev->mac);
    sbprintf(sb, ", link %s\nAddress:  ", dev->link_up(dev) ? "up" : "down");
    print_ip(sb, my_ip);
    int bits = 0;
    for (uint32_t m = my_mask; m & 0x80000000u; m <<= 1) bits++;
    sbprintf(sb, "/%d, gateway ", bits);
    print_ip(sb, my_gw);
    sbprintf(sb, "\nFrames:   %u in, %u out", st.rx, st.tx);
    if (st.tx_failed) sbprintf(sb, " (%u failed)", st.tx_failed);
    sbprintf(sb, "; %u dropped, %u for others, %u fragments, %u bad at the card\n",
             st.dropped, st.not_ours, st.fragments, dev->rx_errors);
    sbprintf(sb, "Pings:    %u answered\n", st.pings_answered);
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

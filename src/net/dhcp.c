// The DHCP client (RFC 2131), run by the net task.
//
//   INIT        nothing yet: send DISCOVER (once the link is up)
//   SELECTING   DISCOVER sent; resent at 4, 8, 16, 32, 64, 64... s
//   REQUESTING  took the first OFFER and asked for it; four tries
//   BOUND       ACK came: the address is ours until T1
//   RENEWING    from T1, ask the server that gave it, straight to it
//   REBINDING   from T2, ask any server, by broadcast
//
// At the lease's end with no answer the address goes and it starts
// over; a NAK does the same at once. Times are kept in 64-bit ticks,
// so a lease of years is fine. The client does not check that nobody
// else already has the address it was given (RFC 5227's probe).
#include "kernel.h"
#include "netpriv.h"
#include "dhcpmsg.h"

enum { D_OFF, D_INIT, D_SELECTING, D_REQUESTING, D_BOUND, D_RENEWING, D_REBINDING };

#define SEC ((uint64_t)TIMER_HZ)
#define NEVER (~(uint64_t)0)
#define MIN_LEASE 10      // seconds; anything shorter is a broken server
#define MIN_RETRY (10 * SEC)  // between tries while renewing or rebinding

static struct {
    int state;
    uint32_t xid;
    int tries;            // messages sent in this state
    uint64_t next;        // when to send again, or do whatever is due
    uint64_t started;     // when this go began, for the secs field
    uint32_t offered, server, last_ip;
    uint64_t t1, t2, expire;  // NEVER for a lease with no end
    int naks, arp_waits;
} d;

static const char* state_name[] = { "off", "starting", "asking", "requesting", "bound", "renewing", "rebinding" };

static void send_msg(int type) {
    dhcp_req_t r = { 0 };
    r.type = type;
    r.xid = d.xid;
    uint64_t secs = (net_now() - d.started) / SEC;
    r.secs = secs > 0xFFFF ? 0xFFFF : (uint16_t)secs;
    memcpy(r.mac, net_dev->mac, ETH_ALEN);
    uint32_t dst = IP_BROADCAST;
    const uint8_t* mac = net_broadcast_mac;
    uint8_t unicast[ETH_ALEN];
    switch (d.state) {
    case D_SELECTING:
        r.broadcast = 1;
        r.requested = d.last_ip;  // the old one back, if it is free
        break;
    case D_REQUESTING:
        r.broadcast = 1;
        r.requested = d.offered;
        r.server = d.server;
        break;
    case D_RENEWING: {
        r.ciaddr = net_cfg.ip;
        uint32_t hop;
        if (ip_route(d.server, &hop) || arp_lookup(hop, unicast)) return;
        dst = d.server;
        mac = unicast;
        break;
    }
    case D_REBINDING:
        r.ciaddr = net_cfg.ip;
        break;
    }
    uint32_t len = dhcp_build(NET_PAYLOAD + UDP_HLEN, &r);
    udp_header(net_cfg.ip, dst, 68, 67, len);
    ip_send(net_cfg.ip, dst, mac, IP_PROTO_UDP, UDP_HLEN + len);
    d.tries++;
}

// 4 s, then doubling to 64 s, each give or take a second (RFC 2131 4.1).
static uint64_t backoff(int tries) {
    uint64_t base = (4 * SEC) << (tries > 4 ? 4 : tries);
    return base - SEC + net_random() % (2 * SEC);
}

static void drop_address(void) {
    ipconf_t none = { 0 };
    net_set_config(&none);
}

void dhcp_start(void) {
    // Ask for the address DHCP gave last, not one that was set by hand.
    if (net_cfg.ip && d.state != D_OFF) d.last_ip = net_cfg.ip;
    drop_address();
    d.state = D_INIT;
    d.next = 0;
    d.naks = 0;
}

void dhcp_stop(void) { d.state = D_OFF; }

static void begin(int state, uint64_t now) {
    if (state == D_RENEWING) d.xid = net_random();  // a new exchange; rebinding carries it on
    if (state == D_SELECTING || state == D_RENEWING) d.started = now;
    d.state = state;
    d.tries = 0;
    d.arp_waits = 0;
    d.next = now;
}

uint64_t dhcp_poll(uint64_t now) {
    if (!net_dev || d.state == D_OFF) return NEVER;
    if (d.state == D_BOUND || d.state == D_RENEWING || d.state == D_REBINDING) {
        if (now >= d.expire) {
            d.last_ip = net_cfg.ip;
            drop_address();
            begin(D_INIT, now);
        } else if (d.state != D_REBINDING && now >= d.t2) begin(D_REBINDING, now);
        else if (d.state == D_BOUND && now >= d.t1) begin(D_RENEWING, now);
    }
    if (now < d.next) return d.next;
    if (!net_dev->link_up(net_dev)) return d.next = now + SEC;  // no use shouting into a dead cable
    switch (d.state) {
    case D_INIT:
        d.xid = net_random();
        begin(D_SELECTING, now);
        // fall through
    case D_SELECTING:
        send_msg(DHCPDISCOVER);
        d.next = now + backoff(d.tries - 1);
        break;
    case D_REQUESTING:
        if (d.tries == 4) { begin(D_INIT, now); return now; }
        send_msg(DHCPREQUEST);
        d.next = now + backoff(d.tries - 1);
        break;
    case D_BOUND:
        d.next = d.t1;
        break;
    case D_RENEWING:
    case D_REBINDING: {
        int sent = d.tries;
        send_msg(DHCPREQUEST);
        uint64_t end = d.state == D_RENEWING ? d.t2 : d.expire;
        if (d.tries == sent && d.arp_waits++ < 3) d.next = now + SEC;  // asked ARP for the server first
        else {
            // Half the time that is left, as RFC 2131 4.4.5 says, but
            // not so often that a dead server is pestered.
            uint64_t wait = (end - now) / 2;
            d.next = now + (wait < MIN_RETRY ? MIN_RETRY : wait);
            d.arp_waits = 0;
        }
        if (d.next > end) d.next = end;
        break;
    }
    }
    return d.next;
}

// The network's natural mask, for a server that gives none.
static uint32_t classful(uint32_t ip) {
    if (ip >> 31 == 0) return 0xFF000000u;
    if (ip >> 30 == 2) return 0xFFFF0000u;
    return 0xFFFFFF00u;
}

static void take_lease(const dhcp_msg_t* m, uint64_t now) {
    uint32_t ip = m->yiaddr, mask = m->mask, inv;
    if (!(m->have & DHCP_HAVE_MASK) || ((~mask) & (~mask + 1)) || mask == 0 || ~mask < 3) mask = classful(ip);
    inv = ~mask;
    if ((ip & inv) == 0 || (ip & inv) == inv) return;  // the network's own address or its broadcast: nonsense
    ipconf_t c = { 0 };
    c.ip = ip;
    c.mask = mask;
    uint32_t gw = m->router;
    if (ip_unicast(gw) && gw != ip && (gw & mask) == (ip & mask)) c.gw = gw;
    for (int i = 0, n = 0; i < 2; i++)
        if (ip_unicast(m->dns[i])) c.dns[n++] = m->dns[i];
    strlcpy(c.domain, m->domain, sizeof c.domain);
    net_set_config(&c);

    if (m->server) d.server = m->server;
    uint32_t lease = m->have & DHCP_HAVE_LEASE ? m->lease : 3600;
    if (lease == 0xFFFFFFFFu) {
        d.t1 = d.t2 = d.expire = NEVER;
    } else {
        if (lease < MIN_LEASE) lease = MIN_LEASE;
        uint64_t l = (uint64_t)lease * SEC;
        uint64_t t2 = m->have & DHCP_HAVE_T2 && m->t2 < lease ? (uint64_t)m->t2 * SEC : l * 7 / 8;
        uint64_t t1 = m->have & DHCP_HAVE_T1 && (uint64_t)m->t1 * SEC <= t2 ? (uint64_t)m->t1 * SEC : l / 2;
        if (t1 > t2) t1 = t2;
        d.t1 = now + t1;
        d.t2 = now + t2;
        d.expire = now + l;
    }
    d.last_ip = ip;
    d.naks = 0;
    begin(D_BOUND, now);
    d.next = d.t1;
}

static void nak(uint64_t now) {
    drop_address();
    begin(D_INIT, now);
    // A server that refuses everything is asked less and less often.
    d.next = now + (d.naks ? SEC << (d.naks > 6 ? 6 : d.naks) : 0);
    d.naks++;
    d.last_ip = 0;  // that one is not to be had
}

void dhcp_input(const uint8_t* p, uint32_t len) {
    if (!net_dev || d.state == D_OFF) return;
    dhcp_msg_t m;
    if (dhcp_parse(p, len, &m) || m.xid != d.xid || memcmp(m.chaddr, net_dev->mac, ETH_ALEN)) return;
    uint64_t now = net_now();
    switch (d.state) {
    case D_SELECTING:
        // The first good offer will do.
        if (m.type != DHCPOFFER || !ip_unicast(m.yiaddr) || !ip_unicast(m.server)) return;
        d.offered = m.yiaddr;
        d.server = m.server;
        begin(D_REQUESTING, now);
        net_kick();
        return;
    case D_REQUESTING:
        if (m.server && m.server != d.server) return;  // another server, answering someone else's request
        if (m.type == DHCPACK && m.yiaddr == d.offered) take_lease(&m, now);
        else if (m.type == DHCPNAK) nak(now);
        return;
    case D_RENEWING:
    case D_REBINDING:
        if (d.state == D_RENEWING && m.server && m.server != d.server) return;
        if (m.type == DHCPACK && ip_unicast(m.yiaddr)) take_lease(&m, now);
        else if (m.type == DHCPNAK) nak(now);
        return;
    }
}

void dhcp_conf(netconf_t* c) {
    c->mode = d.state == D_OFF ? NETCONF_STATIC : NETCONF_DHCP;
    c->server = d.state >= D_REQUESTING ? d.server : 0;
    c->lease = 0;
    if (d.state >= D_BOUND) {
        uint64_t now = net_now();
        c->lease = d.expire == NEVER ? -1 : d.expire > now ? (int)((d.expire - now) / SEC) : 0;
    }
}

static void print_ip(sbuf_t* sb, uint32_t ip) {
    sbprintf(sb, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

void dhcp_format(sbuf_t* sb) {
    sbprintf(sb, "DHCP:     ");
    if (d.state == D_OFF) { sbprintf(sb, "off (the address was set by hand)\n"); return; }
    if (d.state < D_REQUESTING) { sbprintf(sb, "asking (no answer yet)\n"); return; }
    if (d.state == D_REQUESTING) {
        sbprintf(sb, "asking ");
        print_ip(sb, d.server);
        sbprintf(sb, " for ");
        print_ip(sb, d.offered);
        sbprintf(sb, "\n");
        return;
    }
    sbprintf(sb, "from ");
    print_ip(sb, d.server);
    if (d.expire == NEVER) sbprintf(sb, ", a lease with no end");
    else {
        uint64_t now = net_now();
        uint32_t s = d.expire > now ? (uint32_t)((d.expire - now) / SEC) : 0;
        if (s >= 3600) sbprintf(sb, ", lease %u h %u m left", s / 3600, s / 60 % 60);
        else if (s >= 60) sbprintf(sb, ", lease %u m %u s left", s / 60, s % 60);
        else sbprintf(sb, ", lease %u s left", s);
    }
    if (d.state != D_BOUND) sbprintf(sb, " (%s)", state_name[d.state]);
    sbprintf(sb, "\n");
}

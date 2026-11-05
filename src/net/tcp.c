// TCP: see tcp.h. Sequence numbers wrap, so they are compared by the
// sign of their difference. Every deadline is in ms by tcp_clock, with
// 0 meaning "not set".
#include "tcp.h"
#include "lib.h"
#include "ringtail/abi.h"

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define LT(a, b) ((int32_t)((a) - (b)) < 0)
#define LE(a, b) ((int32_t)((a) - (b)) <= 0)
#define GT(a, b) ((int32_t)((a) - (b)) > 0)
#define GE(a, b) ((int32_t)((a) - (b)) >= 0)

#define EPHEMERAL_FIRST 49152
#define IW_BYTES 14600  // RFC 6928: the first window, at most 10 segments

tcp_stats_t tcp_stats;
static tcp_t* conns;
static int nconns;

typedef struct {
    uint16_t sport, dport;
    uint32_t seq, ack, wnd, mss;
    int flags;
    const uint8_t* data;
    uint32_t dlen;
} seg_t;

static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }

static uint32_t at(uint32_t now, uint32_t ms) { return now + ms ? now + ms : 1; }
static int due(uint32_t t, uint32_t now) { return t && GE(now, t); }

static uint16_t checksum(uint32_t src, uint32_t dst, const uint8_t* seg, uint32_t len) {
    uint8_t ph[12];
    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = 6;
    put16(ph + 10, len);
    uint32_t s = 0;
    for (int i = 0; i < 12; i += 2) s += get16(ph + i);
    for (; len > 1; seg += 2, len -= 2) s += get16(seg);
    if (len) s += (uint32_t)seg[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

// RFC 6528: a clock ticking every 4 microseconds, plus a keyed hash of
// the connection, so a stranger cannot guess where one starts.
static uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

static uint32_t new_isn(const tcp_t* c) {
    static uint32_t secret;
    if (!secret) secret = tcp_random() | 1;
    uint32_t h = mix(mix(mix(c->lip ^ secret) ^ c->rip) ^ ((uint32_t)c->lport << 16 | c->rport));
    return h + tcp_clock() * 250;
}

const char* tcp_state_name(int s) {
    static const char* names[] = { "CLOSED", "LISTEN", "SYN-SENT", "SYN-RECEIVED", "ESTABLISHED", "FIN-WAIT-1",
                                   "FIN-WAIT-2", "CLOSE-WAIT", "CLOSING", "LAST-ACK", "TIME-WAIT" };
    return s >= 0 && s < (int)ARRAY_SIZE(names) ? names[s] : "?";
}

tcp_t* tcp_first(void) { return conns; }

// ---- The table ----

static void free_buffers(tcp_t* c) {
    if (c->sbuf) tcp_free(c->sbuf);
    if (c->rbuf) tcp_free(c->rbuf);
    c->sbuf = c->rbuf = 0;
    c->slen = c->rlen = 0;
    c->nooo = 0;
}

static int alloc_buffers(tcp_t* c) {
    if (!c->sbuf) c->sbuf = tcp_alloc(TCP_BUF);
    if (!c->rbuf) c->rbuf = tcp_alloc(TCP_BUF);
    if (c->sbuf && c->rbuf) return 0;
    free_buffers(c);
    return -ENOMEM;
}

static void unlink_child(tcp_t* c) {
    tcp_t* l = c->parent;
    for (tcp_t** p = &l->accept_q; *p; p = &(*p)->accept_next)
        if (*p == c) { *p = c->accept_next; break; }
    c->accept_next = 0;
    c->parent = 0;
    l->children--;
}

static void destroy(tcp_t* c) {
    if (c->parent) unlink_child(c);
    for (tcp_t** p = &conns; *p; p = &(*p)->next)
        if (*p == c) { *p = c->next; break; }
    nconns--;
    free_buffers(c);
    tcp_free(c);
}

// Make room by ending the oldest TIME-WAIT that nobody holds.
static int recycle(void) {
    tcp_t* old = 0;
    for (tcp_t* c = conns; c; c = c->next)
        if (c->state == TCP_TIME_WAIT && c->refs == 0 && (!old || LT(c->linger_at, old->linger_at))) old = c;
    if (!old) return 0;
    destroy(old);
    return 1;
}

tcp_t* tcp_new(void) {
    if (nconns >= TCP_MAX_CONNS && !recycle()) return 0;
    tcp_t* c = tcp_alloc(sizeof *c);
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    c->mss = TCP_MSS_DEFAULT;
    c->rto = TCP_RTO_INIT;
    c->next = conns;
    conns = c;
    nconns++;
    return c;
}

// The connection a segment from rip:rport to lip:lport belongs to.
static tcp_t* lookup(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport) {
    for (tcp_t* c = conns; c; c = c->next)
        if (c->lport == lport && c->rport == rport && c->rip == rip && c->lip == lip &&
            c->state != TCP_LISTEN && c->state != TCP_CLOSED) return c;
    return 0;
}

static tcp_t* listener(uint16_t port) {
    for (tcp_t* c = conns; c; c = c->next)
        if (c->state == TCP_LISTEN && c->lport == port) return c;
    return 0;
}

// The connection is over. Free it if nobody holds it; else tell its owner.
static void finish(tcp_t* c, int err) {
    c->state = TCP_CLOSED;
    c->rexmit_at = c->ack_at = c->linger_at = 0;
    if (err) c->err = err;
    if (c->refs == 0) destroy(c);
    else tcp_wake(c);
}

// ---- Sending segments ----

// The window to offer. Its right edge never moves left, and moves
// right only by a worthwhile amount (RFC 1122 4.2.3.3, avoiding the
// silly window syndrome).
static uint32_t offered(const tcp_t* c) { return GT(c->rcv_adv, c->rcv_nxt) ? c->rcv_adv - c->rcv_nxt : 0; }

static int window_grows(const tcp_t* c) {
    return c->rbuf && TCP_BUF - c->rlen >= offered(c) + MIN(TCP_BUF / 2, c->mss);
}

static uint32_t offer_window(tcp_t* c) {
    if (window_grows(c)) c->rcv_adv = c->rcv_nxt + (TCP_BUF - c->rlen);
    return offered(c);
}

static void header(uint8_t* s, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, int flags,
                   uint32_t wnd, uint32_t hlen) {
    put16(s, sport);
    put16(s + 2, dport);
    put32(s + 4, seq);
    put32(s + 8, ack);
    s[12] = (uint8_t)(hlen / 4 << 4);
    s[13] = (uint8_t)flags;
    put16(s + 14, wnd);
    put16(s + 16, 0);
    put16(s + 18, 0);
}

static void transmit(const uint8_t* mac, uint32_t src, uint32_t dst, uint8_t* s, uint32_t len, int flags) {
    put16(s + 16, checksum(src, dst, s, len));
    tcp_stats.out++;
    if (flags & RST) tcp_stats.resets_out++;
    tcp_transmit(mac, src, dst, len);
}

// A segment with no connection behind it: the answer to a stray one.
static void send_bare(const uint8_t* mac, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                      uint32_t seq, uint32_t ack, int flags) {
    uint8_t* s = tcp_txbuf();
    header(s, sport, dport, seq, ack, flags, 0, TCP_HLEN);
    transmit(mac, src, dst, s, TCP_HLEN, flags);
}

// n bytes from the send buffer starting at seq, a SYN carrying our
// MSS, or neither.
static void send_seg(tcp_t* c, uint32_t seq, uint32_t n, int flags) {
    uint8_t* s = tcp_txbuf();
    uint32_t hl = TCP_HLEN + (flags & SYN ? 4 : 0);
    header(s, c->lport, c->rport, seq, flags & ACK ? c->rcv_nxt : 0, flags, flags & RST ? 0 : offer_window(c), hl);
    if (flags & SYN) {
        s[20] = 2;
        s[21] = 4;
        put16(s + 22, TCP_MSS);
    }
    if (n) {
        uint32_t i = (c->shead + (seq - c->sseq)) % TCP_BUF, first = MIN(n, TCP_BUF - i);
        memcpy(s + hl, c->sbuf + i, first);
        memcpy(s + hl + first, c->sbuf, n - first);
    }
    if (flags & ACK) {
        c->ack_now = 0;
        c->ack_at = 0;
        c->segs_unacked = 0;
    }
    transmit(c->mac, c->lip, c->rip, s, hl + n, flags);
}

// A bare ACK or a reset says snd_max, not snd_nxt: after a timeout
// snd_nxt goes back to resend, and a sequence number the peer has
// already passed would get the segment thrown away, its ACK unread
// (and answered with another such ACK, back and forth for ever).
static void send_ack(tcp_t* c) { send_seg(c, c->snd_max, 0, ACK); }
static void send_rst(tcp_t* c) { send_seg(c, c->snd_max, 0, RST | ACK); }

static void arm(tcp_t* c, uint32_t now) { c->rexmit_at = at(now, c->rto); }

// Send what there is to send: a SYN, data and a FIN as far as the
// windows allow, and an ACK if one is due and nothing else carried it.
static void output(tcp_t* c) {
    uint32_t now = tcp_clock();
    switch (c->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
        return;
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED: {
        int flags = SYN | (c->state == TCP_SYN_RECEIVED ? ACK : 0);
        if (c->snd_nxt == c->iss) {
            send_seg(c, c->iss, 0, flags);
            if (!c->retries) {  // Karn: never time a segment that was sent twice
                c->rtt_on = 1;
                c->rtt_seq = c->iss + 1;
                c->rtt_at = now;
            }
            c->snd_nxt = c->snd_max = c->iss + 1;
            if (!c->rexmit_at) arm(c, now);
        } else if (c->ack_now && c->state == TCP_SYN_RECEIVED) {
            send_ack(c);  // for a segment out of the window (RFC 9293 3.10.7.4)
        }
        c->ack_now = 0;
        return;
    }
    case TCP_TIME_WAIT:
        if (c->ack_now) send_ack(c);
        return;
    }
    uint32_t fin_seq = c->sseq + c->slen;
    for (;;) {
        uint32_t off = c->snd_nxt - c->sseq;
        uint32_t unsent = off < c->slen ? c->slen - off : 0;
        uint32_t flight = c->snd_nxt - c->snd_una, wnd = MIN(c->snd_wnd, c->cwnd);
        uint32_t n = MIN(unsent, MIN(wnd > flight ? wnd - flight : 0, c->mss));
        // No small segments while an ACK is on its way, unless that is
        // all there is to send (RFC 1122 4.2.3.4, the sender's side of
        // the silly window syndrome).
        if (n < c->mss && n < unsent && flight && n < c->max_wnd / 2) n = 0;
        if (!n && unsent && c->probe) n = 1;  // into a closed window, to hear when it opens
        c->probe = 0;
        int fin = c->shut_wr && c->snd_nxt + n == fin_seq;
        if (!n && !fin) break;
        if (!c->rtt_on && GE(c->snd_nxt, c->snd_max)) {
            c->rtt_on = 1;
            c->rtt_seq = c->snd_nxt + n + (uint32_t)fin;
            c->rtt_at = now;
        }
        send_seg(c, c->snd_nxt, n, ACK | (fin ? FIN : 0) | (n && n == unsent ? PSH : 0));
        c->snd_nxt += n + (uint32_t)fin;
        if (GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        if (!c->rexmit_at) arm(c, now);
        if (fin) break;
    }
    if (c->ack_now) send_ack(c);
    // A closed window with data waiting: the timer will probe it.
    if (!c->rexmit_at && !c->snd_wnd && c->snd_nxt == c->snd_una && c->slen > c->snd_nxt - c->sseq) arm(c, now);
}

// Send again the first segment not yet acknowledged, for a fast
// retransmit or a partial ACK in recovery.
static void resend_first(tcp_t* c) {
    uint32_t off = c->snd_una - c->sseq, fin_seq = c->sseq + c->slen;
    uint32_t n = off < c->slen ? MIN(MIN(c->slen - off, c->mss), c->snd_max - c->snd_una) : 0;
    int fin = c->shut_wr && c->snd_una + n == fin_seq && LT(fin_seq, c->snd_max);
    if (!n && !fin) return;
    send_seg(c, c->snd_una, n, ACK | (fin ? FIN : 0));
    tcp_stats.retransmits++;
    c->rtt_on = 0;
}

static void challenge_ack(tcp_t* c) {
    tcp_stats.challenge_acks++;
    c->ack_now = 1;
    output(c);
}

// ---- Round trips (RFC 6298) ----

static void set_rto(tcp_t* c) {
    uint32_t r = c->srtt ? c->srtt + MAX(10u, 4 * c->rttvar) : TCP_RTO_INIT;
    c->rto = MIN(MAX(r, (uint32_t)TCP_RTO_MIN), (uint32_t)TCP_RTO_MAX);
}

static void rtt_sample(tcp_t* c, uint32_t r) {
    if (!c->srtt) {
        c->srtt = r ? r : 1;
        c->rttvar = r / 2;
    } else {
        uint32_t d = c->srtt > r ? c->srtt - r : r - c->srtt;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + r) / 8;
        if (!c->srtt) c->srtt = 1;
    }
    set_rto(c);
}

// ---- The calls ----

static int port_held(uint32_t port) {  // by a socket someone has open, or a listener's connections
    for (tcp_t* c = conns; c; c = c->next)
        if (c->lport == port && (c->refs > 0 || c->parent)) return 1;
    return 0;
}

static int port_used(uint32_t port) {
    for (tcp_t* c = conns; c; c = c->next)
        if (c->lport == port) return 1;
    return 0;
}

int tcp_bind(tcp_t* c, int port) {
    if (c->lport || c->state != TCP_CLOSED || c->used) return -EINVAL;
    if (port < 0 || port > 65535) return -EINVAL;
    if (port) {
        if (port_held((uint32_t)port)) return -EADDRINUSE;
        c->lport = (uint16_t)port;
        return port;
    }
    // A free port, from somewhere unpredictable.
    uint32_t span = 65536 - EPHEMERAL_FIRST, start = tcp_random() % span;
    for (uint32_t i = 0; i < span; i++) {
        uint32_t p = EPHEMERAL_FIRST + (start + i) % span;
        if (!port_used(p)) {
            c->lport = (uint16_t)p;
            return (int)p;
        }
    }
    return -EADDRINUSE;
}

int tcp_listen(tcp_t* c, int backlog) {
    if (c->state != TCP_LISTEN && (c->state != TCP_CLOSED || c->used)) return -EINVAL;
    if (!c->lport) {
        int r = tcp_bind(c, 0);
        if (r < 0) return r;
    }
    c->backlog = backlog < 1 ? 1 : MIN(backlog, TCP_BACKLOG_MAX);
    c->state = TCP_LISTEN;
    return 0;
}

static uint32_t peer_mss(uint32_t m) { return m ? MIN(MAX(m, 64u), (uint32_t)TCP_MSS) : TCP_MSS_DEFAULT; }

// The first segment's fields, for either end.
static void start(tcp_t* c) {
    c->iss = new_isn(c);
    c->snd_una = c->snd_nxt = c->snd_max = c->recover = c->iss;
    c->sseq = c->iss + 1;
    c->rto = TCP_RTO_INIT;
}

int tcp_connect(tcp_t* c, uint32_t lip, uint32_t rip, int rport, const uint8_t* mac) {
    if (c->state == TCP_LISTEN) return -EINVAL;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) return -EALREADY;
    if (c->state != TCP_CLOSED || c->used) return -EISCONN;
    if (rport <= 0 || rport > 65535) return -EINVAL;
    if (!c->lport) {
        int r = tcp_bind(c, 0);
        if (r < 0) return r;
    }
    for (tcp_t* t = conns; t; t = t->next)
        if (t != c && t->lport == c->lport && t->rport == rport && t->rip == rip && t->lip == lip) return -EADDRINUSE;
    if (alloc_buffers(c)) return -ENOMEM;
    // Start afresh: a socket whose connect failed may try again.
    tcp_t keep = *c;
    memset(c, 0, sizeof *c);
    c->next = keep.next;
    c->refs = keep.refs;
    c->lport = keep.lport;
    c->sbuf = keep.sbuf;
    c->rbuf = keep.rbuf;
    c->lip = lip;
    c->rip = rip;
    c->rport = (uint16_t)rport;
    if (mac) memcpy(c->mac, mac, 6);
    c->mss = TCP_MSS_DEFAULT;
    start(c);
    c->state = TCP_SYN_SENT;
    tcp_stats.active++;
    output(c);
    return 0;
}

tcp_t* tcp_accept(tcp_t* l) {
    tcp_t* c = l->accept_q;
    if (c) unlink_child(c);
    return c;
}

int tcp_send(tcp_t* c, const uint8_t* buf, uint32_t len) {
    if (c->err) return c->err;
    if (c->shut_wr) return -EPIPE;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return -ENOTCONN;
    uint32_t n = MIN(len, TCP_BUF - c->slen);
    uint32_t i = (c->shead + c->slen) % TCP_BUF, first = MIN(n, TCP_BUF - i);
    memcpy(c->sbuf + i, buf, first);
    memcpy(c->sbuf, buf + first, n - first);
    c->slen += n;
    if (n) output(c);
    return (int)n;
}

int tcp_recv(tcp_t* c, uint8_t* buf, uint32_t len) {
    if (c->rlen) {
        uint32_t n = MIN(len, c->rlen), first = MIN(n, TCP_BUF - c->rhead);
        memcpy(buf, c->rbuf + c->rhead, first);
        memcpy(buf + first, c->rbuf, n - first);
        c->rhead = (c->rhead + n) % TCP_BUF;
        c->rlen -= n;
        // Tell a sender we had held back that there is room again.
        int open = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
        if (open && !c->fin_in && offered(c) < TCP_BUF / 2 && window_grows(c)) {
            c->ack_now = 1;
            output(c);
        }
        return (int)n;
    }
    if (c->err) return c->err;
    if (c->fin_in || c->shut_rd) return 0;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) return -EAGAIN;
    if (c->state == TCP_LISTEN || (c->state == TCP_CLOSED && !c->used)) return -ENOTCONN;
    if (c->state == TCP_CLOSED) return 0;
    return -EAGAIN;
}

int tcp_shutdown(tcp_t* c, int how) {
    if (how < 0 || how > 2) return -EINVAL;
    if (c->state == TCP_LISTEN || c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED ||
        (c->state == TCP_CLOSED && !c->used)) return -ENOTCONN;
    if (how != 1 && !c->shut_rd) {
        c->shut_rd = 1;
        c->rlen = 0;
        c->nooo = 0;
    }
    if (how != 0 && !c->shut_wr) {
        c->shut_wr = 1;
        if (c->state == TCP_ESTABLISHED) c->state = TCP_FIN_WAIT_1;
        else if (c->state == TCP_CLOSE_WAIT) c->state = TCP_LAST_ACK;
        output(c);
    }
    tcp_wake(c);
    return 0;
}

static void kill_children(tcp_t* l) {
    for (tcp_t *c = conns, *next; c; c = next) {
        next = c->next;
        if (c->parent != l) continue;
        send_rst(c);
        destroy(c);
        next = conns;  // the list changed under us: start over
    }
}

void tcp_close(tcp_t* c) {
    switch (c->state) {
    case TCP_LISTEN:
        kill_children(c);
        destroy(c);
        return;
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        destroy(c);
        return;
    case TCP_TIME_WAIT:
        c->orphan = 1;
        free_buffers(c);
        return;
    }
    if (c->rlen || c->nooo) {  // data it was sent will never be read: say so (RFC 2525 2.17)
        send_rst(c);
        destroy(c);
        return;
    }
    c->orphan = 1;
    if (c->state == TCP_SYN_RECEIVED) {  // a connect given up half way
        send_rst(c);
        destroy(c);
        return;
    }
    tcp_shutdown(c, 1);
    if (c->state == TCP_FIN_WAIT_2) c->linger_at = at(tcp_clock(), TCP_FIN_WAIT_2_MS);
}

void tcp_abort(tcp_t* c, int err) {
    if (c->state == TCP_LISTEN) kill_children(c);
    else if (c->state >= TCP_SYN_RECEIVED && c->state != TCP_TIME_WAIT) send_rst(c);
    finish(c, err);
}

int tcp_events(const tcp_t* c) {
    if (c->state == TCP_LISTEN) return c->accept_q ? TCP_EV_IN : 0;
    int ev = 0;
    if (c->err) ev |= TCP_EV_ERR | TCP_EV_IN | TCP_EV_OUT;
    if (c->rlen || c->fin_in || c->shut_rd) ev |= TCP_EV_IN;
    if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) && !c->shut_wr && TCP_BUF - c->slen >= TCP_BUF / 4)
        ev |= TCP_EV_OUT;
    if (c->state == TCP_CLOSED) ev |= TCP_EV_HUP | (c->used ? TCP_EV_IN : 0);
    if (c->fin_in && c->shut_wr) ev |= TCP_EV_HUP;
    return ev;
}

// ---- Receiving ----

static uint32_t option_mss(const uint8_t* o, uint32_t n) {
    for (uint32_t i = 0; i < n;) {
        if (o[i] == 0) break;          // end of options
        if (o[i] == 1) { i++; continue; }  // padding
        if (i + 1 >= n || o[i + 1] < 2 || i + o[i + 1] > n) break;
        if (o[i] == 2 && o[i + 1] == 4) return get16(o + i + 2);
        i += o[i + 1];
    }
    return 0;
}

static void reset_reply(uint32_t src, uint32_t dst, const seg_t* s, const uint8_t* mac) {
    if (s->flags & RST) return;
    if (s->flags & ACK) {
        send_bare(mac, dst, src, s->dport, s->sport, s->ack, 0, RST);
    } else {
        uint32_t len = s->dlen + !!(s->flags & SYN) + !!(s->flags & FIN);
        send_bare(mac, dst, src, s->dport, s->sport, 0, s->seq + len, RST | ACK);
    }
}

static void ring_put(tcp_t* c, uint32_t off, const uint8_t* p, uint32_t n) {
    uint32_t i = (c->rhead + off) % TCP_BUF, first = MIN(n, TCP_BUF - i);
    memcpy(c->rbuf + i, p, first);
    memcpy(c->rbuf, p + first, n - first);
}

// Note that [s, e) has come early, merging it with the ranges it
// touches. With too many gaps the highest range is forgotten (its
// bytes will come again).
static void add_range(tcp_t* c, uint32_t s, uint32_t e) {
    tcp_range_t out[TCP_OOO + 1];
    int n = 0, i = 0;
    while (i < c->nooo && LT(c->ooo[i].end, s)) out[n++] = c->ooo[i++];
    while (i < c->nooo && LE(c->ooo[i].start, e)) {
        if (LT(c->ooo[i].start, s)) s = c->ooo[i].start;
        if (GT(c->ooo[i].end, e)) e = c->ooo[i].end;
        i++;
    }
    out[n].start = s;
    out[n++].end = e;
    while (i < c->nooo) out[n++] = c->ooo[i++];
    c->nooo = MIN(n, TCP_OOO);
    memcpy(c->ooo, out, sizeof(tcp_range_t) * (uint32_t)c->nooo);
}

// The bytes from rcv_nxt are in: take any early ranges they reach.
static void merge(tcp_t* c) {
    while (c->nooo && LE(c->ooo[0].start, c->rcv_nxt)) {
        if (GT(c->ooo[0].end, c->rcv_nxt)) {
            uint32_t d = c->ooo[0].end - c->rcv_nxt;
            c->rlen += d;
            c->rcv_nxt += d;
        }
        c->nooo--;
        for (int i = 0; i < c->nooo; i++) c->ooo[i] = c->ooo[i + 1];
    }
}

static void enter_time_wait(tcp_t* c, uint32_t now) {
    c->state = TCP_TIME_WAIT;
    c->rexmit_at = 0;
    c->linger_at = at(now, TCP_TIME_WAIT_MS);
    if (c->refs == 0) free_buffers(c);
    tcp_wake(c);
}

static void fin_arrived(tcp_t* c, uint32_t now) {
    c->rcv_nxt++;
    c->fin_in = 1;
    c->fin_early = 0;
    c->ack_now = 1;
    if (c->state == TCP_ESTABLISHED) c->state = TCP_CLOSE_WAIT;
    else if (c->state == TCP_FIN_WAIT_1) c->state = TCP_CLOSING;
    else if (c->state == TCP_FIN_WAIT_2) enter_time_wait(c, now);
    tcp_wake(c);
}

// Data (and perhaps a FIN after it) at seq, already found to be in
// the window.
static void data_in(tcp_t* c, uint32_t seq, const uint8_t* p, uint32_t n, int fin, uint32_t now) {
    uint32_t end = seq + n;  // where the FIN is, if there is one
    if (LT(seq, c->rcv_nxt)) {  // the start we have already
        uint32_t d = c->rcv_nxt - seq;
        p += MIN(d, n);
        n -= MIN(d, n);
        seq = c->rcv_nxt;
    }
    uint32_t room_end = c->rcv_nxt + (TCP_BUF - c->rlen);
    if (n && GT(seq + n, room_end)) {  // the end there is no room for
        n = GT(room_end, seq) ? room_end - seq : 0;
        fin = 0;
    }
    if (n && seq == c->rcv_nxt) {
        if (!c->shut_rd) {
            ring_put(c, c->rlen, p, n);
            c->rlen += n;
        }
        c->rcv_nxt += n;
        if (c->nooo) {
            merge(c);
            c->ack_now = 1;  // a gap filled: say so at once
        }
        // An ACK for every second segment, or after a moment (RFC 5681 4.2).
        if (++c->segs_unacked >= 2) c->ack_now = 1;
        else if (!c->ack_at) c->ack_at = at(now, TCP_DELACK);
        tcp_wake(c);
    } else if (n) {
        if (!c->shut_rd) {
            ring_put(c, c->rlen + (seq - c->rcv_nxt), p, n);
            add_range(c, seq, seq + n);
        }
        c->ack_now = 1;  // a duplicate ACK, for the sender's fast retransmit
    }
    if (fin && GT(end, c->rcv_nxt)) {  // it came before data we lack
        c->fin_early = 1;
        c->fin_early_seq = end;
        c->ack_now = 1;
    } else if (fin && end == c->rcv_nxt) {
        fin_arrived(c, now);
    }
    if (c->fin_early && c->fin_early_seq == c->rcv_nxt) fin_arrived(c, now);
}

static void established(tcp_t* c) {
    c->state = TCP_ESTABLISHED;
    c->used = 1;
    // RFC 5681 3.1: one segment if the SYN had to be sent again.
    c->cwnd = c->retries ? c->mss : MIN(10 * c->mss, MAX(2 * c->mss, (uint32_t)IW_BYTES));
    c->ssthresh = 0x7FFFFFFF;
    if (c->parent) {
        tcp_t** p = &c->parent->accept_q;
        while (*p) p = &(*p)->accept_next;
        *p = c;
        tcp_wake(c->parent);
    }
    tcp_wake(c);
}

// The acknowledgment in a segment for a synchronized connection.
// Returns 0 to go on with the segment, -1 to drop it, 1 if c is gone.
static int ack_in(tcp_t* c, const seg_t* s, uint32_t dlen, uint32_t now) {
    uint32_t ack = s->ack;
    if (GT(ack, c->snd_max)) {  // for something never sent
        c->ack_now = 1;
        output(c);
        return -1;
    }
    if (LT(ack, c->snd_una) && c->snd_una - ack > MAX(c->max_wnd, (uint32_t)TCP_MSS)) {  // far too old (RFC 5961 5.2)
        challenge_ack(c);
        return -1;
    }
    uint32_t old_wnd = c->snd_wnd;
    int newly = GT(ack, c->snd_una);
    if (LT(c->snd_wl1, s->seq) || (c->snd_wl1 == s->seq && LE(c->snd_wl2, ack))) {
        c->snd_wnd = s->wnd;
        c->snd_wl1 = s->seq;
        c->snd_wl2 = ack;
        if (s->wnd > c->max_wnd) c->max_wnd = s->wnd;
    }
    if (newly) {
        uint32_t acked = ack - c->snd_una;
        uint32_t d = GT(ack, c->sseq) ? MIN(ack - c->sseq, c->slen) : 0;
        c->shead = (c->shead + d) % TCP_BUF;
        c->slen -= d;
        c->sseq += d;
        c->snd_una = ack;
        if (LT(c->snd_nxt, ack)) c->snd_nxt = ack;
        if (c->rtt_on && GE(ack, c->rtt_seq)) {
            rtt_sample(c, now - c->rtt_at);
            c->rtt_on = 0;
        } else if (c->retries) {
            set_rto(c);  // the backoff is over
        }
        c->retries = 0;
        int rearmed = 0;
        if (c->recovering) {
            if (GE(ack, c->recover)) {  // all that was out when the loss was found
                c->cwnd = MIN(c->ssthresh, c->snd_max - c->snd_una + c->mss);
                c->recovering = 0;
            } else {  // a partial ACK: the next hole (RFC 6582 3.2)
                resend_first(c);
                c->cwnd = (c->cwnd > acked ? c->cwnd - acked : 0) + c->mss;
                c->rexmit_at = at(now, c->rto);
                rearmed = 1;
            }
        } else if (c->cwnd < c->ssthresh) {
            c->cwnd += MIN(acked, c->mss);  // slow start
        } else {
            c->cwnd += MAX(1u, c->mss * c->mss / c->cwnd);  // congestion avoidance
        }
        if (c->cwnd > 4 * TCP_BUF) c->cwnd = 4 * TCP_BUF;
        c->dupacks = 0;
        if (c->snd_una == c->snd_max) c->rexmit_at = 0;
        else if (!rearmed) c->rexmit_at = at(now, c->rto);
        tcp_wake(c);
        // Our FIN acknowledged?
        if (c->shut_wr && !c->slen && c->snd_una == c->sseq + 1) {
            if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_FIN_WAIT_2;
                if (c->orphan) c->linger_at = at(now, TCP_FIN_WAIT_2_MS);
            } else if (c->state == TCP_CLOSING) {
                enter_time_wait(c, now);
            } else if (c->state == TCP_LAST_ACK) {
                finish(c, 0);
                return 1;
            }
        }
    } else if (ack == c->snd_una && !dlen && !(s->flags & FIN) && s->wnd == old_wnd && c->snd_max != c->snd_una) {
        // A duplicate ACK: something after snd_una came, but not it.
        if (++c->dupacks == 3 && !c->recovering && GT(ack, c->recover)) {
            c->ssthresh = MAX((c->snd_max - c->snd_una) / 2, 2 * c->mss);
            c->recover = c->snd_max;
            c->recovering = 1;
            resend_first(c);
            c->cwnd = c->ssthresh + 3 * c->mss;
            c->rexmit_at = at(now, c->rto);
        } else if (c->dupacks > 3 && c->recovering) {
            c->cwnd += c->mss;  // another segment has left the network
        }
    }
    if (!c->snd_wnd) c->retries = 0;  // it answers our probes: it is alive, only full
    if (!old_wnd && c->snd_wnd && !newly && c->snd_nxt != c->snd_una) {
        c->snd_nxt = c->snd_una;  // the probe went into a closed window: send it again
        c->rtt_on = 0;
    }
    return 0;
}

static void listen_in(tcp_t* l, uint32_t src, uint32_t dst, const seg_t* s, const uint8_t* mac) {
    if (s->flags & RST) return;
    if (s->flags & ACK) { reset_reply(src, dst, s, mac); return; }
    if (!(s->flags & SYN)) return;
    // A full backlog drops the SYN quietly: the client will send it again.
    tcp_t* c = l->children < l->backlog ? tcp_new() : 0;
    if (c && alloc_buffers(c)) { destroy(c); c = 0; }
    if (!c) { tcp_stats.syn_dropped++; return; }
    c->state = TCP_SYN_RECEIVED;
    c->used = 1;
    c->lip = dst;
    c->lport = s->dport;
    c->rip = src;
    c->rport = s->sport;
    if (mac) memcpy(c->mac, mac, 6);
    c->parent = l;
    l->children++;
    c->irs = s->seq;
    c->rcv_nxt = s->seq + 1;
    c->rcv_adv = c->rcv_nxt + TCP_BUF;
    c->mss = peer_mss(s->mss);
    start(c);
    c->snd_wnd = c->max_wnd = s->wnd;
    c->snd_wl1 = s->seq;
    c->snd_wl2 = c->iss;
    tcp_stats.passive++;
    output(c);
}

static void syn_sent_in(tcp_t* c, const seg_t* s, const uint8_t* mac) {
    int fl = s->flags;
    if ((fl & ACK) && (LE(s->ack, c->iss) || GT(s->ack, c->snd_max))) {
        if (!(fl & RST)) send_bare(mac, c->lip, c->rip, c->lport, c->rport, s->ack, 0, RST);
        return;
    }
    if (fl & RST) {
        if (fl & ACK) {
            tcp_stats.resets_in++;
            finish(c, -ECONNREFUSED);
        }
        return;
    }
    if (!(fl & SYN)) return;
    if (mac) memcpy(c->mac, mac, 6);
    c->irs = s->seq;
    c->rcv_nxt = s->seq + 1;
    c->rcv_adv = c->rcv_nxt + TCP_BUF;
    c->mss = peer_mss(s->mss);
    c->snd_wnd = c->max_wnd = s->wnd;
    c->snd_wl1 = s->seq;
    if (!(fl & ACK)) {
        // Both ends opened at once (RFC 9293 3.5): our SYN again, with an ACK.
        c->state = TCP_SYN_RECEIVED;
        c->snd_wl2 = c->iss;
        c->snd_nxt = c->iss;
        output(c);
        return;
    }
    uint32_t now = tcp_clock();
    c->snd_una = s->ack;
    c->snd_wl2 = s->ack;
    if (c->rtt_on && GE(s->ack, c->rtt_seq)) rtt_sample(c, now - c->rtt_at);
    else if (c->retries) set_rto(c);
    c->rtt_on = 0;
    established(c);
    c->retries = 0;
    c->rexmit_at = 0;
    c->ack_now = 1;
    if (s->dlen || (fl & FIN)) data_in(c, s->seq + 1, s->data, s->dlen, fl & FIN, now);
    output(c);
}

static void segment_in(tcp_t* c, seg_t* s, const uint8_t* mac) {
    if (c->state == TCP_SYN_SENT) { syn_sent_in(c, s, mac); return; }
    uint32_t now = tcp_clock();
    int fl = s->flags;
    uint32_t seq = s->seq, dlen = s->dlen;

    // In the window? (RFC 9293 3.10.7.4)
    uint32_t len = dlen + !!(fl & SYN) + !!(fl & FIN), wnd = offered(c);
    int ok;
    // A bare ACK may sit at the window's right edge, where a sender
    // that filled the window puts it.
    if (!len) ok = wnd ? GE(seq, c->rcv_nxt) && LE(seq, c->rcv_nxt + wnd) : seq == c->rcv_nxt;
    else ok = wnd && ((GE(seq, c->rcv_nxt) && LT(seq, c->rcv_nxt + wnd)) ||
                      (GE(seq + len - 1, c->rcv_nxt) && LT(seq + len - 1, c->rcv_nxt + wnd)));
    if (!ok && !wnd && seq == c->rcv_nxt) {
        // A probe of our closed window: its ACK and window still count.
        ok = 1;
        dlen = 0;
        fl &= ~FIN;
        c->ack_now = 1;
    }
    if (!ok) {
        if (c->state == TCP_SYN_RECEIVED && (fl & (SYN | ACK | RST)) == SYN && seq == c->irs) {
            send_seg(c, c->iss, 0, SYN | ACK);  // the peer's SYN again: our SYN-ACK was lost
            c->rtt_on = 0;
        } else if (!(fl & RST)) {
            c->ack_now = 1;
            output(c);
            if (c->state == TCP_TIME_WAIT && (fl & FIN)) c->linger_at = at(now, TCP_TIME_WAIT_MS);
        }
        return;
    }
    if (fl & RST) {
        // Only the exact next sequence number resets (RFC 5961 3.2);
        // anything else in the window just earns an ACK.
        if (seq != c->rcv_nxt) { challenge_ack(c); return; }
        tcp_stats.resets_in++;
        int err = 0;
        if (c->state == TCP_SYN_RECEIVED) err = -ECONNREFUSED;
        else if (c->state <= TCP_CLOSE_WAIT) err = -ECONNRESET;
        finish(c, err);
        return;
    }
    if (fl & SYN) { challenge_ack(c); return; }  // never right once synchronized (RFC 5961 4.2)
    if (!(fl & ACK)) return;
    if (mac) memcpy(c->mac, mac, 6);
    if (c->state == TCP_SYN_RECEIVED) {
        if (!GT(s->ack, c->snd_una) || GT(s->ack, c->snd_max)) {
            send_bare(c->mac, c->lip, c->rip, c->lport, c->rport, s->ack, 0, RST);
            return;
        }
        established(c);
    }
    int r = ack_in(c, s, dlen, now);
    if (r) return;
    int open = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
    if (open && (dlen || (fl & FIN))) {
        if (c->orphan && dlen && GT(seq + dlen, c->rcv_nxt)) {  // nobody is left to read it
            send_rst(c);
            finish(c, 0);
            return;
        }
        data_in(c, seq, s->data, dlen, fl & FIN, now);
    }
    output(c);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t* p, uint32_t len, const uint8_t* mac) {
    tcp_stats.in++;
    uint32_t hl = len >= TCP_HLEN ? (p[12] >> 4) * 4u : 0;
    if (hl < TCP_HLEN || hl > len || checksum(src, dst, p, len) != 0) { tcp_stats.bad++; return; }
    seg_t s;
    s.sport = get16(p);
    s.dport = get16(p + 2);
    s.seq = get32(p + 4);
    s.ack = get32(p + 8);
    s.flags = p[13] & 0x3F;
    s.wnd = get16(p + 14);
    s.data = p + hl;
    s.dlen = len - hl;
    s.mss = (s.flags & SYN) ? option_mss(p + TCP_HLEN, hl - TCP_HLEN) : 0;
    if (!s.sport || !s.dport) { tcp_stats.bad++; return; }
    tcp_t* c = lookup(dst, s.dport, src, s.sport);
    if (c) { segment_in(c, &s, mac); return; }
    tcp_t* l = listener(s.dport);
    if (l) { listen_in(l, src, dst, &s, mac); return; }
    reset_reply(src, dst, &s, mac);  // nobody here
}

void tcp_icmp_error(uint32_t src, uint32_t dst, const uint8_t* th, int err) {
    tcp_t* c = lookup(src, get16(th), dst, get16(th + 2));
    // Only an attempt to connect gives up on one (RFC 1122 4.2.3.9),
    // and only for the SYN it sent (RFC 5927).
    if (c && c->state == TCP_SYN_SENT && get32(th + 4) == c->iss) finish(c, err);
}

// ---- Timers ----

static int rexmit_due(tcp_t* c) {
    int syn = c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED;
    if (++c->retries > (syn ? TCP_SYN_RETRIES : TCP_RETRIES)) {
        finish(c, -ETIMEDOUT);
        return 1;
    }
    tcp_stats.retransmits++;
    if (!syn && c->snd_wnd) {  // a loss: back to one segment (RFC 5681 3.1)
        c->ssthresh = MAX((c->snd_max - c->snd_una) / 2, 2 * c->mss);
        c->cwnd = c->mss;
    }
    c->rto = MIN(c->rto * 2, (uint32_t)TCP_RTO_MAX);
    c->recover = c->snd_max;
    c->recovering = 0;
    c->dupacks = 0;
    c->rtt_on = 0;
    c->snd_nxt = syn ? c->iss : c->snd_una;
    if (!syn && !c->snd_wnd) c->probe = 1;
    output(c);
    return 0;
}

uint32_t tcp_timers(void) {
    uint32_t now = tcp_clock();
    for (tcp_t *c = conns, *next; c; c = next) {
        next = c->next;
        if (due(c->linger_at, now)) {  // TIME-WAIT is over, or the peer's FIN never came
            c->linger_at = 0;
            if (c->state == TCP_FIN_WAIT_2) send_rst(c);
            finish(c, 0);
            continue;
        }
        if (due(c->rexmit_at, now)) {
            c->rexmit_at = 0;
            if (rexmit_due(c)) continue;
        }
        if (due(c->ack_at, now)) {
            c->ack_at = 0;
            c->ack_now = 1;
            output(c);
        }
    }
    uint32_t next_ms = ~0u;
    for (tcp_t* c = conns; c; c = c->next) {
        uint32_t t[3] = { c->linger_at, c->rexmit_at, c->ack_at };
        for (int i = 0; i < 3; i++)
            if (t[i]) next_ms = MIN(next_ms, GT(t[i], now) ? t[i] - now : 0);
    }
    return next_ms;
}

void tcp_addr_lost(uint32_t ip) {
    for (tcp_t *c = conns, *next; c; c = next) {
        next = c->next;
        if (c->lip == ip && c->state != TCP_LISTEN && c->state != TCP_CLOSED)
            finish(c, c->state == TCP_TIME_WAIT ? 0 : -ECONNABORTED);
    }
}

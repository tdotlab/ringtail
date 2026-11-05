// Host tests for the TCP engine (src/net/tcp.c), built 32-bit under the
// address and undefined-behaviour sanitizers. This file is the platform:
// a clock it moves by hand, a simulated network that delays, loses,
// reorders, duplicates and corrupts segments, and malloc with a count
// so a run that leaks a connection fails. Both ends of a connection
// live in the one engine (as they do over loopback), at A and B. A
// third address, R, is a raw peer: what is sent there is captured, and
// the tests write its segments by hand to poke at the state machine.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tcp.h"
#include "ringtail/abi.h"

static int passed, failed;
#define CHECK(cond, what) do { \
    if (cond) passed++; \
    else { failed++; printf("  FAIL  %s (line %d)\n", what, __LINE__); } \
} while (0)

#define A 0x0A000001u
#define B 0x0A000002u
#define R 0x0A000063u     // the raw peer
#define HOLE 0x0A0000FEu  // nothing comes back from here

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_GE(a, b) ((int32_t)((a) - (b)) >= 0)

static const uint8_t MAC[6] = { 2, 0, 0, 0, 0, 1 };

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t rd32(const uint8_t* p) { return rd16(p) << 16 | rd16(p + 2); }
static void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v); }

// ---- The platform ----

static uint32_t now;
static uint32_t rng = 1, net_rng = 1;
static uint32_t xs(uint32_t* s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}
static int live_allocs;
static uint8_t txbuf[TCP_HLEN + 4 + TCP_MSS];

uint32_t tcp_clock(void) { return now; }
uint32_t tcp_random(void) { return xs(&rng); }
void* tcp_alloc(uint32_t size) { live_allocs++; return malloc(size); }
void tcp_free(void* p) { live_allocs--; free(p); }
uint8_t* tcp_txbuf(void) { return txbuf; }
void tcp_wake(tcp_t* c) { (void)c; }

// The network. Each segment gets a delivery time; ones in flight are
// kept sorted by it.
typedef struct pkt { struct pkt* next; uint32_t t, src, dst, len; uint8_t d[TCP_HLEN + 4 + TCP_MSS]; } pkt_t;
static pkt_t* flight;
static struct {
    uint32_t delay, jitter;           // ms, one way
    uint32_t loss, dup, corrupt;      // per 1000 segments
    int (*filter)(uint32_t src, uint32_t dst, const uint8_t* seg, uint32_t len);  // 1 drops it
} net;

// What went to R and HOLE, in order.
typedef struct { uint32_t t, src, dst, len; uint8_t d[TCP_HLEN + 4 + TCP_MSS]; } cap_t;
#define NCAP 4096
static cap_t cap[NCAP];
static int ncap, nhole;
static uint32_t hole_at[64];

static void enqueue(uint32_t src, uint32_t dst, const uint8_t* d, uint32_t len, uint32_t t) {
    pkt_t* p = malloc(sizeof *p);
    p->src = src;
    p->dst = dst;
    p->len = len;
    p->t = t;
    memcpy(p->d, d, len);
    pkt_t** q = &flight;
    while (*q && !SEQ_LT(t, (*q)->t)) q = &(*q)->next;
    p->next = *q;
    *q = p;
}

static int trace;  // print every segment: ./test_tcp seed N

void tcp_transmit(const uint8_t* mac, uint32_t src, uint32_t dst, uint32_t len) {
    (void)mac;
    if (trace)
        printf("%8u %c>%c seq %10u ack %10u %c%c%c%c%c wnd %5u len %4u\n", now, 'A' + (src & 0xFF) - 1,
               'A' + (dst & 0xFF) - 1, rd32(txbuf + 4), rd32(txbuf + 8), txbuf[13] & SYN ? 'S' : '.',
               txbuf[13] & ACK ? 'A' : '.', txbuf[13] & FIN ? 'F' : '.', txbuf[13] & RST ? 'R' : '.',
               txbuf[13] & PSH ? 'P' : '.', rd16(txbuf + 14), len - (uint32_t)(txbuf[12] >> 4) * 4);
    if (dst == R) {
        if (ncap < NCAP) {
            cap[ncap].t = now;
            cap[ncap].src = src;
            cap[ncap].dst = dst;
            cap[ncap].len = len;
            memcpy(cap[ncap].d, txbuf, len);
            ncap++;
        }
        return;
    }
    if (dst == HOLE) {
        if (nhole < 64) hole_at[nhole] = now;
        nhole++;
        return;
    }
    if (net.filter && net.filter(src, dst, txbuf, len)) return;
    if (xs(&net_rng) % 1000 < net.loss) return;
    int copies = xs(&net_rng) % 1000 < net.dup ? 2 : 1;
    for (int i = 0; i < copies; i++) {
        uint8_t d[sizeof txbuf];
        memcpy(d, txbuf, len);
        if (xs(&net_rng) % 1000 < net.corrupt) d[xs(&net_rng) % len] ^= (uint8_t)(1 + xs(&net_rng) % 255);
        uint32_t t = now + net.delay + (net.jitter ? xs(&net_rng) % (net.jitter + 1) : 0);
        enqueue(src, dst, d, len, t);
    }
}

// One millisecond: what has arrived is delivered, then the timers run.
static void step(void) {
    while (flight && SEQ_GE(now, flight->t)) {
        pkt_t* p = flight;
        flight = p->next;
        tcp_input(p->src, p->dst, p->d, p->len, MAC);
        free(p);
    }
    tcp_timers();
    now++;
}

static void run(uint32_t ms) {
    for (uint32_t end = now + ms; SEQ_LT(now, end);) step();
}

#define WAIT(cond, ms) ({ \
    uint32_t _end = now + (ms); \
    int _ok; \
    while (!(_ok = !!(cond)) && SEQ_LT(now, _end)) step(); \
    _ok; })

// With nothing for the applications to do: jump from one event to the
// next, trusting what tcp_timers says is due next. Stops when the
// table is empty.
static void drain(uint32_t ms) {
    uint32_t end = now + ms;
    while (tcp_first() && SEQ_LT(now, end)) {
        while (flight && SEQ_GE(now, flight->t)) {
            pkt_t* p = flight;
            flight = p->next;
            tcp_input(p->src, p->dst, p->d, p->len, MAC);
            free(p);
        }
        uint32_t r = tcp_timers(), next = r == ~0u ? end : now + (r ? r : 1);
        if (flight && SEQ_LT(flight->t, next)) next = flight->t;
        if (SEQ_GE(now, next)) next = now + 1;
        now = next;
    }
}

static int nconns(void) {
    int n = 0;
    for (tcp_t* c = tcp_first(); c; c = c->next) n++;
    return n;
}

static void reset_world(uint32_t start) {
    while (flight) {
        pkt_t* p = flight;
        flight = p->next;
        free(p);
    }
    memset(&net, 0, sizeof net);
    net.delay = 5;
    ncap = nhole = 0;
    now = start;
}

// What the descriptor layer does: refs counts the holders.
static tcp_t* sock(void) {
    tcp_t* c = tcp_new();
    if (c) c->refs = 1;
    return c;
}
static void drop(tcp_t* c) {
    c->refs = 0;
    tcp_close(c);
}
static tcp_t* server(int port, int backlog) {
    tcp_t* l = sock();
    tcp_bind(l, port);
    tcp_listen(l, backlog);
    return l;
}
static tcp_t* accept1(tcp_t* l) {
    tcp_t* c = tcp_accept(l);
    if (c) c->refs = 1;
    return c;
}
// The server learns of a connection half a round trip after the client.
#define accept_wait(l) (WAIT((l)->accept_q != 0, 2000), accept1(l))

// The end of every test: nothing left, nothing leaked.
static void all_gone(const char* what) {
    drain(30 * 60 * 1000);
    char m[128];
    snprintf(m, sizeof m, "%s: every connection is gone and its memory freed", what);
    CHECK(!tcp_first() && live_allocs == 0, m);
    if (tcp_first() || live_allocs) {
        for (tcp_t* c = tcp_first(); c; c = c->next)
            printf("        left: %s port %u refs %d\n", tcp_state_name(c->state), c->lport, c->refs);
        while (tcp_first()) {  // start the next test clean
            tcp_t* c = tcp_first();
            c->state = TCP_CLOSED;
            drop(c);
        }
        live_allocs = 0;
    }
}

// Bytes of a test stream: byte i of the stream with this seed.
static uint8_t sbyte(uint32_t seed, uint32_t i) { return (uint8_t)(((i ^ seed) * 2654435761u) >> 24); }

// Read whatever is there; count it and check it against the stream.
// Returns what tcp_recv last said.
static int sink(tcp_t* c, uint32_t seed, uint32_t* got, int* bad, uint32_t most) {
    uint8_t buf[5000];
    int r = -EAGAIN;
    while (most && (r = tcp_recv(c, buf, most < sizeof buf ? most : sizeof buf)) > 0) {
        most -= (uint32_t)r;
        for (int i = 0; i < r; i++)
            if (buf[i] != sbyte(seed, *got + (uint32_t)i)) (*bad)++;
        *got += (uint32_t)r;
    }
    return r;
}

static void source(tcp_t* c, uint32_t seed, uint32_t* sent, uint32_t total) {
    uint8_t buf[5000];
    while (*sent < total) {
        uint32_t n = total - *sent < sizeof buf ? total - *sent : sizeof buf;
        for (uint32_t i = 0; i < n; i++) buf[i] = sbyte(seed, *sent + i);
        int r = tcp_send(c, buf, n);
        if (r <= 0) break;
        *sent += (uint32_t)r;
    }
}

// ---- The raw peer ----

static uint16_t tcp_sum(uint32_t src, uint32_t dst, const uint8_t* s, uint32_t len) {
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += rd16(s + i);
    if (len & 1) sum += (uint32_t)s[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// A segment from R to A. mss 0 sends no option.
static void raw(uint32_t sport, uint32_t dport, uint32_t seq, uint32_t ack, int flags, uint32_t wnd,
                const void* data, uint32_t n, uint32_t mss) {
    uint8_t s[2000];
    uint32_t hl = TCP_HLEN + (mss ? 4 : 0);
    memset(s, 0, hl);
    wr16(s, sport);
    wr16(s + 2, dport);
    wr32(s + 4, seq);
    wr32(s + 8, ack);
    s[12] = (uint8_t)(hl / 4 << 4);
    s[13] = (uint8_t)flags;
    wr16(s + 14, wnd);
    if (mss) {
        s[20] = 2;
        s[21] = 4;
        wr16(s + 22, mss);
    }
    if (n) memcpy(s + hl, data, n);
    wr16(s + 16, tcp_sum(R, A, s, hl + n));
    tcp_input(R, A, s, hl + n, MAC);
}

static uint32_t c_seq(const cap_t* c) { return rd32(c->d + 4); }
static uint32_t c_ack(const cap_t* c) { return rd32(c->d + 8); }
static int c_flags(const cap_t* c) { return c->d[13]; }
static uint32_t c_wnd(const cap_t* c) { return rd16(c->d + 14); }
static uint32_t c_hl(const cap_t* c) { return (uint32_t)(c->d[12] >> 4) * 4; }
static uint32_t c_dlen(const cap_t* c) { return c->len - c_hl(c); }
static const cap_t* last_cap(void) { return ncap ? &cap[ncap - 1] : 0; }

// A connection from A to the raw peer, opened by hand. The peer's
// sequence numbers start at irs + 1.
static tcp_t* raw_open(uint32_t rport, uint32_t irs, uint32_t mss, uint32_t* iss) {
    tcp_t* c = sock();
    tcp_connect(c, A, R, (int)rport, MAC);
    const cap_t* syn = last_cap();
    *iss = c_seq(syn);
    raw(rport, c->lport, irs, *iss + 1, SYN | ACK, 65535, 0, 0, mss);
    return c;
}

// ---- Scenarios ----

static void test_basic(void) {
    printf("handshake, transfer, half-close, close\n");
    reset_world(1000);
    tcp_t* l = server(80, 4);
    CHECK(l->state == TCP_LISTEN && l->lport == 80, "a listener on port 80");
    tcp_t* c = sock();
    CHECK(tcp_connect(c, A, B, 80, MAC) == 0 && c->state == TCP_SYN_SENT, "connect sends a SYN");
    CHECK(c->lport >= 49152, "the client gets an ephemeral port");
    CHECK(!(tcp_events(c) & TCP_EV_OUT), "nothing can be sent while connecting");
    CHECK(WAIT(tcp_events(l) & TCP_EV_IN, 100), "the listener has a connection to accept");
    tcp_t* s = accept1(l);
    CHECK(s && s->state == TCP_ESTABLISHED && c->state == TCP_ESTABLISHED, "both ends established");
    CHECK(s && s->rport == c->lport && s->lip == B && s->rip == A, "the accepted end has the right addresses");
    CHECK(!tcp_accept(l), "only one to accept");
    CHECK(c->mss == TCP_MSS && s->mss == TCP_MSS, "each end takes the other's MSS of 1460");
    CHECK(tcp_events(c) & TCP_EV_OUT, "the client can send");

    uint32_t sent = 0, got = 0;
    int bad = 0;
    source(c, 1, &sent, 100000);
    CHECK(sent == TCP_BUF, "the first send fills the buffer and no more");
    for (int i = 0; i < 5000 && got < 100000; i++) {
        source(c, 1, &sent, 100000);
        sink(s, 1, &got, &bad, ~0u);
        step();
    }
    CHECK(got == 100000 && !bad, "100 KB arrive intact and in order");
    CHECK(c->srtt >= 9 && c->srtt <= 20, "the round trip is measured (10 ms)");
    CHECK(c->rto == TCP_RTO_MIN, "the RTO sits at its floor of 200 ms");

    CHECK(tcp_shutdown(c, 1) == 0 && c->state == TCP_FIN_WAIT_1, "shutdown for writing sends a FIN");
    uint8_t buf[100];
    CHECK(WAIT(tcp_recv(s, buf, sizeof buf) == 0, 100), "the server reads the end");
    run(20);
    CHECK(s->state == TCP_CLOSE_WAIT && c->state == TCP_FIN_WAIT_2, "CLOSE-WAIT and FIN-WAIT-2");
    CHECK(tcp_send(c, buf, 1) == -EPIPE, "nothing more can be sent after shutdown");
    CHECK(tcp_send(s, (const uint8_t*)"bye", 3) == 3, "the other way is still open");
    CHECK(WAIT(tcp_recv(c, buf, sizeof buf) == 3, 100) && !memcmp(buf, "bye", 3), "and the client reads it");
    drop(s);
    CHECK(WAIT(c->state == TCP_TIME_WAIT, 100), "the client is in TIME-WAIT");
    run(20);
    CHECK(nconns() == 2, "the server's end is gone once its FIN is acknowledged");
    CHECK(tcp_recv(c, buf, sizeof buf) == 0, "the client reads the end");
    CHECK(tcp_events(c) & TCP_EV_HUP, "and nothing more will come either way");
    drop(c);
    run(TCP_TIME_WAIT_MS - 100);
    CHECK(nconns() == 2, "TIME-WAIT lasts 30 s");
    run(200);
    CHECK(nconns() == 1, "then it is over");
    drop(l);
    all_gone("basic");
}

static void test_refused(void) {
    printf("refused, then tried again\n");
    reset_world(5000);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 81, MAC);
    CHECK(WAIT(c->state == TCP_CLOSED, 100) && c->err == -ECONNREFUSED, "a closed port refuses");
    CHECK((tcp_events(c) & (TCP_EV_ERR | TCP_EV_HUP)) == (TCP_EV_ERR | TCP_EV_HUP), "poll sees the error");
    CHECK(tcp_send(c, (const uint8_t*)"x", 1) == -ECONNREFUSED, "send reports it");
    tcp_t* l = server(81, 1);
    CHECK(tcp_connect(c, A, B, 81, MAC) == 0, "a failed socket may connect again");
    CHECK(WAIT(c->state == TCP_ESTABLISHED, 100) && !c->err, "and this time it works");
    WAIT(tcp_events(l) & TCP_EV_IN, 100);
    tcp_t* s = accept1(l);
    drop(c);
    drop(s);
    drop(l);
    all_gone("refused");
}

static void test_syn_timeout(void) {
    printf("SYN retransmission and timeout\n");
    reset_world(0xFFFFF000u);  // the clock wraps during this one
    uint32_t t0 = now;
    tcp_t* c = sock();
    tcp_connect(c, A, HOLE, 80, MAC);
    CHECK(tcp_timers() == 1000, "the first SYN waits 1 s");
    CHECK(WAIT(c->state == TCP_CLOSED, 40000), "the connect gives up");
    CHECK(c->err == -ETIMEDOUT, "with ETIMEDOUT");
    CHECK(nhole == 5, "after five SYNs");
    static const uint32_t want[5] = { 0, 1000, 3000, 7000, 15000 };
    int ok = 1;
    for (int i = 0; i < 5 && i < nhole; i++) ok &= hole_at[i] - t0 == want[i];
    CHECK(ok, "sent at 0, 1, 3, 7 and 15 s");
    CHECK(now - t0 >= 31000 && now - t0 <= 31002, "given up at 31 s");
    drop(c);
    all_gone("syn timeout");
}

static void test_mss(void) {
    printf("the MSS option, both ways\n");
    reset_world(100);
    tcp_t* c = sock();
    tcp_connect(c, A, R, 7, MAC);
    const cap_t* syn = last_cap();
    CHECK(syn && c_flags(syn) == SYN && c_hl(syn) == 24, "the SYN carries one option");
    CHECK(syn && syn->d[20] == 2 && syn->d[21] == 4 && rd16(syn->d + 22) == TCP_MSS, "the MSS option says 1460");
    CHECK(syn && c_wnd(syn) == TCP_BUF, "and the window is the whole buffer");
    uint32_t iss = c_seq(syn);
    raw(7, c->lport, 5000, iss + 1, SYN | ACK, 65535, 0, 0, 536);
    CHECK(c->state == TCP_ESTABLISHED && c->mss == 536, "a peer's MSS of 536 is taken");
    const cap_t* ack = last_cap();
    CHECK(ack && c_flags(ack) == ACK && c_ack(ack) == 5001 && c_seq(ack) == iss + 1, "the third segment of the handshake");
    int before = ncap;
    uint8_t data[5000];
    memset(data, 'm', sizeof data);
    CHECK(tcp_send(c, data, sizeof data) == 5000, "5000 bytes are taken");
    uint32_t total = 0, biggest = 0, expect = iss + 1;
    int inorder = 1;
    for (int i = before; i < ncap; i++) {
        total += c_dlen(&cap[i]);
        if (c_dlen(&cap[i]) > biggest) biggest = c_dlen(&cap[i]);
        inorder &= c_seq(&cap[i]) == expect;
        expect += c_dlen(&cap[i]);
    }
    CHECK(total == 5000 && ncap - before == 10, "the first window sends them all, in ten segments");
    CHECK(biggest == 536 && inorder, "none bigger than 536, in order");
    CHECK(c_flags(last_cap()) == (ACK | PSH), "the last one pushes");
    raw(7, c->lport, 5001, iss + 1 + 5000, ACK, 65535, 0, 0, 0);
    CHECK(c->snd_una == iss + 5001 && c->slen == 0 && !c->rexmit_at, "all acknowledged, the timer off");
    c->state = TCP_CLOSED;  // nothing to say goodbye to
    drop(c);
    all_gone("mss");
}

static void test_out_of_order(void) {
    printf("out-of-order data, an early FIN, sequence numbers wrapping\n");
    reset_world(100);
    uint32_t iss, irs = 0xFFFFFF00u, p = irs + 1;
    tcp_t* c = raw_open(7, irs, 1460, &iss);
    uint8_t d[500];
    for (int i = 0; i < 500; i++) d[i] = (uint8_t)i;
    int n0 = ncap;
    raw(7, c->lport, p + 100, iss + 1, ACK, 65535, d + 100, 100, 0);
    CHECK(ncap == n0 + 1 && c_ack(last_cap()) == p, "a segment after a gap: a duplicate ACK at once");
    CHECK(c->nooo == 1 && c->rlen == 0, "kept aside");
    raw(7, c->lport, p + 200, iss + 1, ACK, 65535, d + 200, 100, 0);
    CHECK(c->nooo == 1, "the next one joins its range");
    raw(7, c->lport, p, iss + 1, ACK, 65535, d, 100, 0);
    CHECK(c_ack(last_cap()) == p + 300 && c->rlen == 300 && c->nooo == 0, "the gap fills: 300 bytes, acknowledged at once");
    CHECK(c->rcv_nxt == irs + 301 && c->rcv_nxt < irs, "rcv_nxt wrapped past zero");
    raw(7, c->lport, p + 400, iss + 1, ACK | FIN, 65535, d + 400, 100, 0);
    CHECK(c->fin_early && !c->fin_in, "a FIN after a gap waits");
    raw(7, c->lport, p + 300, iss + 1, ACK, 65535, d + 300, 100, 0);
    CHECK(c->fin_in && c->state == TCP_CLOSE_WAIT, "it counts once the gap fills");
    CHECK(c_ack(last_cap()) == p + 501, "everything and the FIN acknowledged");
    uint8_t buf[600];
    int r = tcp_recv(c, buf, sizeof buf);
    CHECK(r == 500 && !memcmp(buf, d, 500), "500 bytes, in order");
    CHECK(tcp_recv(c, buf, sizeof buf) == 0, "then the end");
    // A copy of something old, overlapping what came: only an ACK.
    n0 = ncap;
    raw(7, c->lport, p + 50, iss + 1, ACK, 65535, d + 50, 100, 0);
    CHECK(ncap == n0 + 1 && c_ack(last_cap()) == p + 501, "an old copy is answered with an ACK");
    c->state = TCP_CLOSED;
    drop(c);
    all_gone("out of order");
}

static void test_ooo_many_gaps(void) {
    printf("more gaps than are remembered\n");
    reset_world(100);
    uint32_t iss, p = 1001;
    tcp_t* c = raw_open(7, 1000, 1460, &iss);
    uint8_t d[1000];
    for (int i = 0; i < 1000; i++) d[i] = (uint8_t)(i * 7);
    // Segments 1, 3, 5, 7, 9 and 11 of 100 bytes each: six gaps.
    for (int k = 11; k >= 1; k -= 2) raw(7, c->lport, p + k * 50u, iss + 1, ACK, 65535, d + k * 50, 50, 0);
    CHECK(c->nooo == TCP_OOO, "four ranges kept");
    // Now the even ones: the gaps fill up to the first range forgotten.
    for (int k = 0; k < 20; k += 2) raw(7, c->lport, p + k * 50u, iss + 1, ACK, 65535, d + k * 50, 50, 0);
    CHECK(c->rcv_nxt == p + 450 && c->rlen == 450, "up to the first range that was forgotten");
    raw(7, c->lport, p, iss + 1, ACK, 65535, d, 1000, 0);  // the sender tries them all again
    uint8_t buf[1100];
    CHECK(tcp_recv(c, buf, sizeof buf) == 1000 && !memcmp(buf, d, 1000), "every byte in the end, in order");
    c->state = TCP_CLOSED;
    drop(c);
    all_gone("many gaps");
}

static void test_rst_rules(void) {
    printf("RFC 5961: resets and SYNs that do not quite fit\n");
    reset_world(100);
    uint32_t iss, p = 7001;
    tcp_t* c = raw_open(7, 7000, 1460, &iss);
    int n0 = ncap;
    uint32_t ch = tcp_stats.challenge_acks;
    raw(7, c->lport, p + 1000, 0, RST, 0, 0, 0, 0);
    CHECK(c->state == TCP_ESTABLISHED, "a reset in the window but not at its start does not reset");
    CHECK(ncap == n0 + 1 && c_flags(last_cap()) == ACK && c_ack(last_cap()) == p, "it gets a challenge ACK");
    CHECK(tcp_stats.challenge_acks == ch + 1, "counted");
    n0 = ncap;
    raw(7, c->lport, p + 100000, 0, RST, 0, 0, 0, 0);
    CHECK(c->state == TCP_ESTABLISHED && ncap == n0, "one outside the window is ignored outright");
    raw(7, c->lport, p, iss + 1, SYN, 65535, 0, 0, 1460);
    CHECK(c->state == TCP_ESTABLISHED && ncap == n0 + 1 && c_ack(last_cap()) == p, "a SYN gets a challenge ACK too");
    n0 = ncap;
    raw(7, c->lport, p, iss + 1 + 100000, ACK, 65535, "x", 1, 0);
    CHECK(c->rlen == 0 && ncap == n0 + 1, "data acknowledging what was never sent is dropped, with an ACK");
    raw(7, c->lport, p, iss + 1 - 200000, ACK, 65535, "x", 1, 0);
    CHECK(c->rlen == 0, "and so is data with an ACK far in the past");
    raw(7, c->lport, p, 0, RST, 0, 0, 0, 0);
    CHECK(c->state == TCP_CLOSED && c->err == -ECONNRESET, "a reset exactly at rcv_nxt resets");
    uint8_t b[4];
    CHECK(tcp_recv(c, b, 4) == -ECONNRESET, "and recv says so");
    drop(c);
    all_gone("rst rules");
}

static void test_strays(void) {
    printf("strays, bad checksums, garbage\n");
    reset_world(100);
    int n0 = ncap;
    raw(1234, 9999, 555, 0, SYN, 1000, 0, 0, 1460);
    CHECK(ncap == n0 + 1 && c_flags(last_cap()) == (RST | ACK) && c_ack(last_cap()) == 556 && c_seq(last_cap()) == 0,
          "a SYN to a closed port: RST|ACK");
    raw(1234, 9999, 555, 4242, ACK, 1000, "abc", 3, 0);
    CHECK(ncap == n0 + 2 && c_flags(last_cap()) == RST && c_seq(last_cap()) == 4242, "an ACK: a RST at its ACK number");
    raw(1234, 9999, 555, 0, RST, 1000, 0, 0, 0);
    CHECK(ncap == n0 + 2, "a reset is never answered");
    tcp_t* l = server(9999, 2);
    raw(1234, 9999, 555, 4242, ACK, 1000, 0, 0, 0);
    CHECK(ncap == n0 + 3 && c_flags(last_cap()) == RST, "an ACK to a listener: a RST");
    uint32_t bad = tcp_stats.bad;
    uint8_t s[40] = { 0 };
    wr16(s, 1);
    wr16(s + 2, 9999);
    s[12] = 0x50;
    s[13] = SYN;
    wr16(s + 16, tcp_sum(R, A, s, 20) ^ 1);
    tcp_input(R, A, s, 20, MAC);
    CHECK(tcp_stats.bad == bad + 1 && ncap == n0 + 3 && l->children == 0, "a bad checksum is dropped");
    s[12] = 0xF0;  // 60 bytes of header in a 20-byte segment
    wr16(s + 16, 0);
    wr16(s + 16, tcp_sum(R, A, s, 20));
    tcp_input(R, A, s, 20, MAC);
    tcp_input(R, A, s, 7, MAC);
    CHECK(tcp_stats.bad == bad + 3, "so is a header longer than the segment, or a stub");
    // Options that lie about their length.
    static const uint8_t opts[][8] = {
        { 2, 4, 0, 0, 1, 1, 1, 1 }, { 2, 0, 0, 0, 0, 0, 0, 0 }, { 2, 9, 1, 1, 1, 1, 1, 1 },
        { 1, 1, 1, 1, 1, 1, 2, 4 }, { 8, 10, 0, 0, 0, 0, 0, 0 }, { 2, 4, 0xFF, 0xFF, 0, 0, 0, 0 },
    };
    for (unsigned k = 0; k < sizeof opts / sizeof opts[0]; k++) {
        memset(s, 0, sizeof s);
        wr16(s, 2000 + k);
        wr16(s + 2, 9999);
        wr32(s + 4, 100);
        s[12] = 0x70;
        s[13] = SYN;
        wr16(s + 14, 1000);
        memcpy(s + 20, opts[k], 8);
        wr16(s + 16, tcp_sum(R, A, s, 28));
        tcp_input(R, A, s, 28, MAC);
    }
    int mss_ok = 1, kids = 0;
    for (tcp_t* c = tcp_first(); c; c = c->next)
        if (c->parent == l) {
            kids++;
            mss_ok &= c->mss >= 64 && c->mss <= TCP_MSS;
        }
    CHECK(kids == 2 && mss_ok, "odd options are survived, and the MSS is kept sane");
    CHECK(tcp_stats.syn_dropped >= 4, "SYNs past the backlog are dropped");
    drop(l);
    all_gone("strays");
}

static void test_delayed_ack(void) {
    printf("delayed ACKs\n");
    reset_world(100);
    uint32_t iss, p = 1;
    tcp_t* c = raw_open(7, 0, 1460, &iss);
    int n0 = ncap;
    raw(7, c->lport, p, iss + 1, ACK, 65535, "a", 1, 0);
    CHECK(ncap == n0, "one segment: no ACK yet");
    run(TCP_DELACK - 1);
    CHECK(ncap == n0, "not for 40 ms");
    run(2);
    CHECK(ncap == n0 + 1 && c_ack(last_cap()) == p + 1, "then it goes");
    raw(7, c->lport, p + 1, iss + 1, ACK, 65535, "b", 1, 0);
    raw(7, c->lport, p + 2, iss + 1, ACK, 65535, "c", 1, 0);
    CHECK(ncap == n0 + 2 && c_ack(last_cap()) == p + 3, "two segments: an ACK at once");
    raw(7, c->lport, p + 3, iss + 1, ACK, 65535, "d", 1, 0);
    CHECK(tcp_send(c, (const uint8_t*)"reply", 5) == 5, "a reply");
    CHECK(ncap == n0 + 3 && c_ack(last_cap()) == p + 4 && c_dlen(last_cap()) == 5, "carries the ACK");
    run(100);
    CHECK(ncap == n0 + 3, "and no separate ACK follows");
    c->state = TCP_CLOSED;
    drop(c);
    all_gone("delayed ack");
}

static void test_icmp(void) {
    printf("ICMP errors\n");
    reset_world(100);
    tcp_t* c = sock();
    tcp_connect(c, A, R, 7, MAC);
    uint8_t th[8];
    memcpy(th, last_cap()->d, 8);
    uint8_t wrong[8];
    memcpy(wrong, th, 8);
    wr32(wrong + 4, rd32(th + 4) + 1);
    tcp_icmp_error(A, R, wrong, -EHOSTUNREACH);
    CHECK(c->state == TCP_SYN_SENT, "an error quoting a sequence number we did not send is ignored");
    tcp_icmp_error(A, R, th, -EHOSTUNREACH);
    CHECK(c->state == TCP_CLOSED && c->err == -EHOSTUNREACH, "one quoting our SYN ends the connect");
    drop(c);
    uint32_t iss;
    c = raw_open(7, 0, 1460, &iss);
    memcpy(th, last_cap()->d, 8);
    tcp_icmp_error(A, R, th, -EHOSTUNREACH);
    CHECK(c->state == TCP_ESTABLISHED, "an established connection rides it out");
    c->state = TCP_CLOSED;
    drop(c);
    all_gone("icmp");
}

static void test_api(void) {
    printf("calls made at the wrong time\n");
    reset_world(100);
    uint8_t b[8];
    tcp_t* c = sock();
    CHECK(tcp_send(c, b, 1) == -ENOTCONN && tcp_recv(c, b, 1) == -ENOTCONN, "send and recv before connecting");
    CHECK(tcp_shutdown(c, 1) == -ENOTCONN && tcp_shutdown(c, 3) == -EINVAL, "shutdown too");
    CHECK(tcp_bind(c, 70000) == -EINVAL, "a port past 65535");
    CHECK(tcp_bind(c, 4000) == 4000 && tcp_bind(c, 4001) == -EINVAL, "bind once");
    tcp_t* d = sock();
    CHECK(tcp_bind(d, 4000) == -EADDRINUSE, "a port someone holds");
    CHECK(tcp_connect(d, A, B, 0, MAC) == -EINVAL, "port 0");
    CHECK(tcp_connect(d, A, HOLE, 80, MAC) == 0, "connect");
    CHECK(tcp_connect(d, A, HOLE, 80, MAC) == -EALREADY, "connect again while connecting");
    CHECK(tcp_listen(d, 1) == -EINVAL, "listen while connecting");
    CHECK(tcp_recv(d, b, 1) == -EAGAIN, "recv while connecting waits");
    CHECK(tcp_listen(c, 0) == 0 && c->backlog == 1, "a backlog of 0 means 1");
    CHECK(tcp_listen(c, 1000) == 0 && c->backlog == TCP_BACKLOG_MAX, "and a big one is capped");
    CHECK(tcp_connect(c, A, B, 80, MAC) == -EINVAL, "a listener cannot connect");
    CHECK(tcp_recv(c, b, 1) == -ENOTCONN && tcp_send(c, b, 1) == -ENOTCONN, "nor send or receive");
    tcp_t* e = sock();
    CHECK(tcp_listen(e, 1) == 0 && e->lport >= 49152, "listen without bind picks a port");
    drop(c);
    drop(d);
    drop(e);
    tcp_t* f = sock();
    CHECK(tcp_bind(f, 4000) == 4000, "a closed socket's port is free again");
    drop(f);
    all_gone("api");
}

// ---- Scenarios over the simulated network ----

static uint32_t drop_seq, drop_at, again_at;
static int dropped;
static int drop_third(uint32_t src, uint32_t dst, const uint8_t* s, uint32_t len) {
    (void)dst;
    uint32_t hl = (uint32_t)(s[12] >> 4) * 4, seq = rd32(s + 4);
    if (src != A || len == hl) return 0;
    if (!dropped && ++drop_at == 30) {  // the 30th data segment, once
        dropped = 1;
        drop_seq = seq;
        drop_at = now;
        return 1;
    }
    if (dropped && seq == drop_seq && !again_at) again_at = now;
    return 0;
}

static void test_fast_retransmit(void) {
    printf("one segment lost: fast retransmit and NewReno\n");
    reset_world(100);
    net.delay = 20;
    net.filter = drop_third;
    drop_at = again_at = dropped = 0;
    tcp_t* l = server(80, 1);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    tcp_t* s = accept1(l);
    uint32_t sent = 0, got = 0, rex = tcp_stats.retransmits;
    int bad = 0;
    for (int i = 0; i < 20000 && got < 300000; i++) {
        source(c, 7, &sent, 300000);
        sink(s, 7, &got, &bad, ~0u);
        step();
    }
    CHECK(got == 300000 && !bad, "300 KB arrive intact");
    CHECK(dropped && again_at, "the lost segment was sent again");
    CHECK(again_at - drop_at < 100, "within a round trip or so, not after a timeout");
    CHECK(tcp_stats.retransmits - rex == 1, "and it was the only one sent twice");
    CHECK(!c->recovering && c->cwnd >= c->mss * 2, "recovery is over, the window intact");
    drop(c);
    drop(s);
    drop(l);
    all_gone("fast retransmit");
}

// Window edges the server offers, checked as they go by.
static uint32_t last_edge;
static int edge_seen, edge_back, edge_small;
static int watch_edges(uint32_t src, uint32_t dst, const uint8_t* s, uint32_t len) {
    (void)dst; (void)len;
    if (src != B || (s[13] & (RST | SYN))) return 0;
    uint32_t edge = rd32(s + 8) + rd16(s + 14);
    if (edge_seen && SEQ_LT(edge, last_edge)) edge_back++;
    if (edge_seen && edge != last_edge && edge - last_edge < TCP_MSS) edge_small++;
    if (!edge_seen || !SEQ_LT(edge, last_edge)) last_edge = edge;
    edge_seen = 1;
    return 0;
}

static void test_zero_window(void) {
    printf("a reader that stops: zero window and probes\n");
    reset_world(100);
    net.delay = 10;
    net.filter = watch_edges;
    edge_seen = edge_back = edge_small = 0;
    tcp_t* l = server(80, 1);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    tcp_t* s = accept1(l);
    uint32_t sent = 0, got = 0, total = 200000;
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        source(c, 3, &sent, total);
        step();
    }
    CHECK(s->rlen == TCP_BUF && c->slen == TCP_BUF && c->snd_wnd == 0, "both buffers full, the window shut");
    CHECK(!(tcp_events(c) & TCP_EV_OUT), "the sender cannot send");
    uint32_t out0 = tcp_stats.out;
    run(10 * 60 * 1000);
    CHECK(c->state == TCP_ESTABLISHED, "ten minutes on, the connection lives");
    CHECK(tcp_stats.out - out0 > 8 && tcp_stats.out - out0 < 60, "probed a few times, backing off to a minute");
    // Now read a little at a time: the window opens by worthwhile steps.
    for (int i = 0; i < 200000 && got < total; i++) {
        source(c, 3, &sent, total);
        if (i % 3 == 0) {
            uint8_t buf[100];
            int r = tcp_recv(s, buf, sizeof buf);
            for (int k = 0; k < r; k++) bad += buf[k] != sbyte(3, got + (uint32_t)k);
            if (r > 0) got += (uint32_t)r;
        }
        step();
    }
    CHECK(got == total && !bad, "everything arrives once the reader reads");
    CHECK(edge_seen && !edge_back, "the window's right edge never moved left");
    CHECK(!edge_small, "nor moved right by less than a segment");
    drop(c);
    drop(s);
    drop(l);
    all_gone("zero window");
}

static void test_simultaneous(void) {
    printf("simultaneous open and close\n");
    reset_world(100);
    net.delay = 30;
    tcp_t* a = sock();
    tcp_t* b = sock();
    tcp_bind(a, 5000);
    tcp_bind(b, 6000);
    tcp_connect(a, A, B, 6000, MAC);
    tcp_connect(b, B, A, 5000, MAC);
    CHECK(WAIT(a->state == TCP_ESTABLISHED && b->state == TCP_ESTABLISHED, 3000), "both open at once and meet");
    uint8_t buf[10];
    tcp_send(a, (const uint8_t*)"ab", 2);
    tcp_send(b, (const uint8_t*)"ba", 2);
    CHECK(WAIT(a->rlen == 2 && b->rlen == 2, 200), "data both ways");
    tcp_recv(a, buf, sizeof buf);
    tcp_recv(b, buf, sizeof buf);
    tcp_shutdown(a, 2);
    tcp_shutdown(b, 2);
    run(35);
    CHECK(a->state == TCP_CLOSING && b->state == TCP_CLOSING, "both FINs cross: CLOSING");
    CHECK(WAIT(a->state == TCP_TIME_WAIT && b->state == TCP_TIME_WAIT, 200), "then both in TIME-WAIT");
    drop(a);
    drop(b);
    all_gone("simultaneous");
}

static void test_unread_close(void) {
    printf("closing with unread data resets\n");
    reset_world(100);
    tcp_t* l = server(80, 1);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    tcp_t* s = accept1(l);
    tcp_send(c, (const uint8_t*)"unread", 6);
    WAIT(s->rlen == 6, 100);
    drop(s);
    CHECK(nconns() == 2, "the server's end is gone at once");
    CHECK(WAIT(c->state == TCP_CLOSED, 100) && c->err == -ECONNRESET, "and the client hears a reset");
    drop(c);
    drop(l);
    all_gone("unread close");
}

static void test_backlog(void) {
    printf("a full backlog\n");
    reset_world(100);
    tcp_t* l = server(80, 2);
    tcp_t* c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = sock();
        tcp_connect(c[i], A, B, 80, MAC);
    }
    run(500);
    int est = 0;
    for (int i = 0; i < 4; i++) est += c[i]->state == TCP_ESTABLISHED;
    CHECK(est == 2 && l->children == 2, "two get in, two wait");
    tcp_t* s[4];
    s[0] = accept1(l);
    s[1] = accept1(l);
    CHECK(s[0] && s[1] && !tcp_accept(l) && l->children == 0, "accepting makes room");
    CHECK(WAIT(c[0]->state + c[1]->state + c[2]->state + c[3]->state == 4 * TCP_ESTABLISHED, 2000),
          "the other two get in when their SYNs come again");
    CHECK(c[2]->cwnd == c[2]->mss, "starting with one segment, their SYNs having been lost");
    s[2] = accept_wait(l);
    s[3] = accept_wait(l);
    for (int i = 0; i < 4; i++) drop(c[i]);
    drop(l);
    for (int i = 0; i < 4; i++) drop(s[i]);
    all_gone("backlog");
}

static void test_listener_close(void) {
    printf("closing a listener with connections waiting\n");
    reset_world(100);
    tcp_t* l = server(80, 4);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(l->accept_q != 0, 100);
    drop(l);
    CHECK(WAIT(c->state == TCP_CLOSED, 100) && c->err == -ECONNRESET, "they are reset");
    drop(c);
    all_gone("listener close");
}

static void test_orphans(void) {
    printf("orphans: FIN-WAIT-2 timeout, data to a closed socket\n");
    reset_world(100);
    tcp_t* l = server(80, 2);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    tcp_t* s = accept1(l);
    drop(c);  // the client goes away; the server never closes
    run(100);
    CHECK(s->state == TCP_CLOSE_WAIT && nconns() == 3, "the client's end lingers in FIN-WAIT-2");
    run(TCP_FIN_WAIT_2_MS);
    CHECK(nconns() == 2, "for 30 s");
    CHECK(s->state == TCP_CLOSED && s->err == -ECONNRESET, "then resets the server's end");
    drop(s);

    c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    s = accept1(l);
    drop(c);
    run(50);
    tcp_send(s, (const uint8_t*)"anyone?", 7);
    CHECK(WAIT(s->state == TCP_CLOSED, 100) && s->err == -ECONNRESET, "data to an orphan is answered with a reset");
    CHECK(nconns() == 2, "and the orphan is gone");
    drop(s);
    drop(l);
    all_gone("orphans");
}

static void test_addr_lost(void) {
    printf("the address goes away\n");
    reset_world(100);
    tcp_t* l = server(80, 2);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 200);
    tcp_t* s = accept1(l);
    tcp_addr_lost(A);
    CHECK(c->state == TCP_CLOSED && c->err == -ECONNABORTED, "connections from it are aborted");
    CHECK(s->state == TCP_ESTABLISHED && l->state == TCP_LISTEN, "others are not");
    drop(c);
    drop(s);
    drop(l);
    all_gone("addr lost");
}

static void test_table_full(void) {
    printf("a full table recycles TIME-WAIT\n");
    reset_world(100);
    tcp_t* l = server(80, 16);
    for (int i = 0; i < 40; i++) {
        tcp_t* c = sock();
        tcp_connect(c, A, B, 80, MAC);
        WAIT(tcp_events(l) & TCP_EV_IN, 100);
        tcp_t* s = accept1(l);
        drop(c);  // the client closes first, and so waits
        WAIT(s->state == TCP_CLOSE_WAIT, 100);
        drop(s);
        run(20);
    }
    CHECK(nconns() == 41, "forty connections in TIME-WAIT");
    tcp_t* held[TCP_MAX_CONNS];
    int n = 0;
    while (n < TCP_MAX_CONNS && (held[n] = sock())) n++;
    CHECK(n == TCP_MAX_CONNS - 1, "new sockets push them out until only held ones are left");
    for (int i = 0; i < n; i++) drop(held[i]);
    drop(l);
    all_gone("table full");
}

static void test_bulk(void) {
    printf("a megabyte at 100 ms round trip\n");
    reset_world(100);
    net.delay = 50;
    tcp_t* l = server(80, 1);
    tcp_t* c = sock();
    tcp_connect(c, A, B, 80, MAC);
    WAIT(tcp_events(l) & TCP_EV_IN, 300);
    tcp_t* s = accept1(l);
    uint32_t sent = 0, got = 0, t0 = now;
    int bad = 0;
    while (got < 1000000 && now - t0 < 60000) {
        source(c, 9, &sent, 1000000);
        sink(s, 9, &got, &bad, ~0u);
        step();
    }
    CHECK(got == 1000000 && !bad, "it arrives");
    // A 16 KB window per 100 ms is 160 KB/s at best: about 6.3 s.
    CHECK(now - t0 < 8000, "at close to what the window allows");
    CHECK(c->srtt >= 95 && c->srtt <= 130 && c->rto >= 200 && c->rto <= 400, "the RTT and RTO are sane");
    drop(c);
    drop(s);
    drop(l);
    all_gone("bulk");
}

// ---- Random runs ----

// Two ends trading streams of random length over a random network.
// Each sends its stream, shuts down for writing, reads to the end and
// closes; sometimes a reader dawdles. Everything must arrive exactly,
// both ends must finish, and nothing may leak.
static int stress_one(uint32_t seed) {
    rng = seed * 2 + 1;
    net_rng = seed * 7 + 3;
    uint32_t r = seed * 0x9E3779B9u + 1;
    reset_world(xs(&r) % 4 ? xs(&r) : 0xFFFFFFFFu - xs(&r) % 100000);
    net.delay = 1 + xs(&r) % 80;
    net.jitter = xs(&r) % 3 ? 0 : xs(&r) % 60;
    net.loss = xs(&r) % 4 ? xs(&r) % 100 : 0;
    net.dup = xs(&r) % 3 ? 0 : xs(&r) % 50;
    net.corrupt = xs(&r) % 3 ? 0 : xs(&r) % 30;
    uint32_t total[2] = { xs(&r) % 4 ? xs(&r) % 300000 : xs(&r) % 100, xs(&r) % 2 ? xs(&r) % 200000 : 0 };
    int slow[2] = { xs(&r) % 4 == 0, xs(&r) % 4 == 0 };

    tcp_t* l = server(80, 4);
    tcp_t* e[2] = { 0, 0 };
    uint32_t sent[2] = { 0, 0 }, got[2] = { 0, 0 }, t0 = now;
    int bad = 0, eof[2] = { 0, 0 }, closed[2] = { 0, 0 }, shut[2] = { 0, 0 }, tries = 0, err = 0;
    while (!(closed[0] && closed[1]) && now - t0 < 30 * 60 * 1000) {
        if (!e[0]) {
            if (++tries > 5) break;
            e[0] = sock();
            tcp_connect(e[0], A, B, 80, MAC);
        }
        if (e[0] && !closed[0] && e[0]->state == TCP_CLOSED && !got[0] && !sent[0] && e[0]->err == -ETIMEDOUT) {
            drop(e[0]);  // never got through: try again, as a program would
            e[0] = 0;
            step();
            continue;
        }
        if (!e[1] && (e[1] = accept1(l))) {}
        for (int i = 0; i < 2; i++) {
            tcp_t* c = e[i];
            if (!c || closed[i]) continue;
            if (c->err) { err = c->err; closed[i] = 1; drop(c); continue; }
            if (c->state == TCP_SYN_SENT) continue;
            source(c, seed + (uint32_t)i, &sent[i], total[i]);
            if (sent[i] == total[i] && !shut[i]) { tcp_shutdown(c, 1); shut[i] = 1; }
            if (!slow[i] || now % 50 == 0) {
                int rr = sink(c, seed + 1 - (uint32_t)i, &got[i], &bad, slow[i] ? 2000 : ~0u);
                if (rr == 0) eof[i] = 1;
            }
            if (eof[i] && shut[i]) { closed[i] = 1; drop(c); }
        }
        step();
    }
    int ok = closed[0] && closed[1] && !err && !bad && got[0] == total[1] && got[1] == total[0];
    drop(l);
    drain(30 * 60 * 1000);
    int clean = !tcp_first() && !live_allocs;
    if (!ok || !clean) {
        printf("  seed %u: delay %u jitter %u loss %u dup %u corrupt %u, %u/%u bytes, slow %d/%d\n", seed, net.delay,
               net.jitter, net.loss, net.dup, net.corrupt, total[0], total[1], slow[0], slow[1]);
        printf("        closed %d/%d err %d bad %d got %u/%u eof %d/%d, %s\n", closed[0], closed[1], err, bad, got[0],
               got[1], eof[0], eof[1], clean ? "clean" : "LEFT OVERS");
        for (tcp_t* c = tcp_first(); c; c = c->next)
            printf("        left: %s port %u refs %d slen %u\n", tcp_state_name(c->state), c->lport, c->refs, c->slen);
        while (tcp_first()) {
            tcp_t* c = tcp_first();
            c->state = TCP_CLOSED;
            drop(c);
        }
        live_allocs = 0;
    }
    return ok && clean;
}

static void test_stress(int runs) {
    printf("%d random runs over a bad network\n", runs);
    int good = 0;
    for (int i = 0; i < runs; i++) good += stress_one((uint32_t)i + 1);
    char m[80];
    snprintf(m, sizeof m, "%d of %d runs delivered every byte and cleaned up", good, runs);
    CHECK(good == runs, m);
}

// Garbage with good checksums at a listener and a connection.
static void test_fuzz(void) {
    printf("fuzzing\n");
    reset_world(100);
    uint32_t r = 99;
    tcp_t* l = server(7, 4);
    uint32_t iss;
    tcp_t* c = raw_open(9, 1000, 1460, &iss);
    for (int i = 0; i < 200000; i++) {
        uint8_t s[100];
        uint32_t len = 20 + xs(&r) % 80;
        for (uint32_t k = 0; k < len; k++) s[k] = (uint8_t)xs(&r);
        wr16(s, xs(&r) % 2 ? 9 : 1 + xs(&r) % 3);
        wr16(s + 2, xs(&r) % 2 ? c->lport : 7);
        if (xs(&r) % 2) wr32(s + 4, c->rcv_nxt + xs(&r) % 200);
        if (xs(&r) % 2) wr32(s + 8, c->snd_una + xs(&r) % 3);
        s[12] = (uint8_t)((5 + xs(&r) % 11) << 4);
        s[16] = s[17] = 0;
        wr16(s + 16, tcp_sum(R, A, s, len));
        tcp_input(R, A, s, len, MAC);
        if (i % 100 == 0) step();
        if (ncap > NCAP - 100) ncap = 0;
        if (c->state == TCP_CLOSED) {  // a lucky reset: start another
            drop(c);
            c = raw_open(9, 1000, 1460, &iss);
        }
    }
    CHECK(1, "200000 random segments survived");
    c->state = TCP_CLOSED;
    drop(c);
    drop(l);
    all_gone("fuzz");
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 2 && !strcmp(argv[1], "seed")) {
        trace = 1;
        return !stress_one((uint32_t)atoi(argv[2]));
    }
    int runs = argc > 1 ? atoi(argv[1]) : 300;
    test_basic();
    test_refused();
    test_syn_timeout();
    test_mss();
    test_out_of_order();
    test_ooo_many_gaps();
    test_rst_rules();
    test_strays();
    test_delayed_ack();
    test_icmp();
    test_api();
    test_fast_retransmit();
    test_zero_window();
    test_simultaneous();
    test_unread_close();
    test_backlog();
    test_listener_close();
    test_orphans();
    test_addr_lost();
    test_table_full();
    test_bulk();
    test_fuzz();
    test_stress(runs);
    printf("test_tcp: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

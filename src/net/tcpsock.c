// TCP in the kernel: what the engine (tcp.c) asks of its platform, and
// the calls behind the TCP system calls. All of it runs with net_lock
// held. A task that has to wait (for an answer, a connection, data or
// room) sleeps on the connection itself. The engine's tcp_wake only
// notes who to wake; the wakeups go out once the engine is done with
// the call that caused them, so a waiter that looks without the lock
// never sees half a change and then sleeps through the rest of it.
#include "kernel.h"
#include "arch.h"
#include "task.h"
#include "mm.h"
#include "fs.h"
#include "netpriv.h"
#include "tcp.h"

// ---- The platform ----

uint32_t tcp_clock(void) { return (uint32_t)(timer_ticks64() * (1000 / TIMER_HZ)); }
uint32_t tcp_random(void) { return net_random(); }
void* tcp_alloc(uint32_t size) { return kmalloc(size); }
void tcp_free(void* p) { kfree(p); }
uint8_t* tcp_txbuf(void) { return NET_PAYLOAD; }

void tcp_transmit(const uint8_t* mac, uint32_t src, uint32_t dst, uint32_t len) {
    // No MAC means the peer came by loopback; if its address is not
    // ours any more, there is nowhere to send this.
    if (!mac && !ip_local(dst)) return;
    ip_send(src, dst, mac, IP_PROTO_TCP, len);
}

static tcp_t* woken[2 * TCP_MAX_CONNS];
static int nwoken;

void tcp_wake(tcp_t* c) {
    for (int i = 0; i < nwoken; i++)
        if (woken[i] == c) return;
    if (nwoken < (int)ARRAY_SIZE(woken)) woken[nwoken++] = c;
    else task_wakeup(c);  // cannot happen with a full table; waking early is safe anyway
}

void tcp_flush_wakeups(void) {
    if (!nwoken) return;
    // A connection woken and then freed in the same call is only a
    // pointer here: task_wakeup compares it and never follows it.
    for (int i = 0; i < nwoken; i++) task_wakeup(woken[i]);
    nwoken = 0;
    poll_notify();
}

// After a call from a task: wake whoever the call concerns, and have
// the net task look again at the deadlines it may have moved.
static void done(void) {
    tcp_flush_wakeups();
    net_kick();
}

// An error is reported once; after it the connection reads as over.
static int report(tcp_t* c, int r) {
    if (r < 0 && r == c->err) c->err = 0;
    return r;
}

static uint32_t deadline(int timeout_ms) {
    uint64_t ticks = timeout_ms > 0 ? ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000 : 0;
    return timer_ticks() + (uint32_t)ticks;
}

// Wait, net_lock held, until ready(c): 0, or -EINTR once the caller is
// killed, or -ETIMEDOUT at tick `until` unless forever. ready is
// checked once more without the lock before sleeping; it may see a
// change half made, but then the wakeup for it is still to come.
static int wait_for(tcp_t* c, int (*ready)(const tcp_t*), int forever, uint32_t until) {
    for (;;) {
        if (ready(c)) return 0;
        if (current->killed) return -EINTR;
        if (!forever && (int32_t)(timer_ticks() - until) >= 0) return -ETIMEDOUT;
        mutex_unlock(&net_lock);
        uint32_t f = irq_save();
        if (!ready(c) && !current->killed) {
            if (forever) task_block(c);
            else task_block_timeout(c, until);
        }
        irq_restore(f);
        mutex_lock(&net_lock);
    }
}

// ---- The calls ----

tcp_t* tcpsock_new(void) {
    mutex_lock(&net_lock);
    tcp_t* c = tcp_new();
    if (c) c->refs = 1;
    mutex_unlock(&net_lock);
    return c;
}

void tcpsock_ref(tcp_t* c) {
    mutex_lock(&net_lock);
    c->refs++;
    mutex_unlock(&net_lock);
}

void tcpsock_unref(tcp_t* c) {
    mutex_lock(&net_lock);
    if (--c->refs == 0) {
        tcp_close(c);  // c may be gone after this
        done();
    }
    mutex_unlock(&net_lock);
}

int tcpsock_bind(tcp_t* c, int port) {
    mutex_lock(&net_lock);
    int r = tcp_bind(c, port);
    mutex_unlock(&net_lock);
    return r;
}

int tcpsock_listen(tcp_t* c, int backlog) {
    mutex_lock(&net_lock);
    int r = tcp_listen(c, backlog);
    mutex_unlock(&net_lock);
    return r;
}

static int answered(const tcp_t* c) { return c->state != TCP_SYN_SENT && c->state != TCP_SYN_RECEIVED; }

int tcpsock_connect(tcp_t* c, uint32_t ip, int port, int timeout_ms) {
    if (port <= 0 || port > 65535) return -EINVAL;
    if (!ip_unicast(ip) && (ip >> 24) != 127) return -EINVAL;
    int r = net_can_send(ip);
    if (r) return r;
    uint32_t until = deadline(timeout_ms);
    mutex_lock(&net_lock);
    uint8_t mac[ETH_ALEN];
    uint32_t hop;
    int local = ip_local(ip);
    r = ip_route(ip, &hop);
    // Waiting for ARP lets go of the lock; c is CLOSED meanwhile, so no
    // segment can reach it, and tcp_connect checks its state afresh.
    if (!r && !local) r = arp_resolve(hop, mac);
    if (!r) r = net_can_send(ip);
    if (!r) r = tcp_connect(c, ip_source(ip), ip, port, local ? 0 : mac);
    if (!r) {
        done();
        r = wait_for(c, answered, timeout_ms <= 0, until);
        if (r == -ETIMEDOUT && !answered(c)) {
            tcp_abort(c, 0);  // our own limit, not TCP's
            done();
        }
        if (!r && c->state == TCP_CLOSED) r = report(c, c->err ? c->err : -ECONNREFUSED);
    }
    mutex_unlock(&net_lock);
    return r;
}

static int acceptable(const tcp_t* l) { return l->accept_q || l->state != TCP_LISTEN; }

int tcpsock_accept(tcp_t* l, tcp_t** out, uint32_t* ip, int* port, int timeout_ms) {
    uint32_t until = deadline(timeout_ms);
    mutex_lock(&net_lock);
    int r = l->state != TCP_LISTEN ? -EINVAL : !l->accept_q && timeout_ms == 0 ? -EAGAIN : 0;
    if (!r) r = wait_for(l, acceptable, timeout_ms < 0, until);
    if (!r && l->state != TCP_LISTEN) r = -EINVAL;
    if (!r) {
        tcp_t* c = tcp_accept(l);
        c->refs = 1;
        *out = c;
        *ip = c->rip;
        *port = c->rport;
    }
    mutex_unlock(&net_lock);
    return r;
}

static int readable(const tcp_t* c) { return tcp_events(c) & TCP_EV_IN; }

int tcpsock_read(tcp_t* c, uint8_t* kbuf, uint32_t len) {
    mutex_lock(&net_lock);
    int r;
    for (;;) {
        r = tcp_recv(c, kbuf, len);
        if (r != -EAGAIN) break;
        if ((r = wait_for(c, readable, 1, 0)) < 0) break;
    }
    if (r > 0) done();  // the window may have opened
    r = report(c, r);
    mutex_unlock(&net_lock);
    return r;
}

static int writable(const tcp_t* c) {
    return c->err || c->shut_wr || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) || c->slen < TCP_BUF;
}

int tcpsock_write(tcp_t* c, const uint8_t* kbuf, uint32_t len) {
    mutex_lock(&net_lock);
    uint32_t sent = 0;
    int r = 0;
    while (sent < len) {
        r = tcp_send(c, kbuf + sent, len - sent);
        if (r < 0) break;
        sent += (uint32_t)r;
        done();
        if (sent < len && (r = wait_for(c, writable, 1, 0)) < 0) break;
    }
    if (!sent) r = report(c, r);
    mutex_unlock(&net_lock);
    return sent ? (int)sent : r;
}

int tcpsock_shutdown(tcp_t* c, int how) {
    mutex_lock(&net_lock);
    int r = tcp_shutdown(c, how);
    done();
    mutex_unlock(&net_lock);
    return r;
}

int tcpsock_addr(tcp_t* c, int peer, uint32_t* ip, int* port) {
    mutex_lock(&net_lock);
    int r = 0;
    if (!peer) {
        *ip = c->lip;
        *port = c->lport;
    } else if (c->rport && c->state != TCP_LISTEN && (c->used || c->state != TCP_CLOSED)) {
        *ip = c->rip;
        *port = c->rport;
    } else r = -ENOTCONN;
    mutex_unlock(&net_lock);
    return r;
}

int tcpsock_events(tcp_t* c) {
    mutex_lock(&net_lock);
    int ev = tcp_events(c);  // the values are poll()'s
    mutex_unlock(&net_lock);
    return ev;
}

// ---- KINFO_NET ----

static void print_end(sbuf_t* sb, uint32_t ip, int port) {
    sbprintf(sb, "%u.%u.%u.%u:%d", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
}

void tcp_format(sbuf_t* sb) {
    int n = 0;
    for (tcp_t* c = tcp_first(); c; c = c->next) n++;
    tcp_stats_t* s = &tcp_stats;
    sbprintf(sb, "TCP:      %d connections; %u segments in, %u out, %u bad; %u retransmitted; "
                 "%u resets in, %u out; %u opened, %u accepted; %u SYNs dropped\n",
             n, s->in, s->out, s->bad, s->retransmits, s->resets_in, s->resets_out, s->active, s->passive,
             s->syn_dropped);
    for (tcp_t* c = tcp_first(); c; c = c->next) {
        if (c->state == TCP_CLOSED && !c->lport) continue;  // a socket not yet used for anything
        sbprintf(sb, "          ");
        if (c->state == TCP_LISTEN) sbprintf(sb, "*:%d", c->lport);
        else print_end(sb, c->lip, c->lport);
        if (c->state != TCP_LISTEN && c->rport) {
            sbprintf(sb, " - ");
            print_end(sb, c->rip, c->rport);
        }
        sbprintf(sb, " %s", tcp_state_name(c->state));
        if (c->state == TCP_LISTEN && c->children) sbprintf(sb, ", %d waiting", c->children);
        if (c->state != TCP_LISTEN && (c->slen || c->rlen)) sbprintf(sb, ", %u out, %u in buffered", c->slen, c->rlen);
        if (!c->refs && c->state != TCP_LISTEN && c->state != TCP_TIME_WAIT) sbprintf(sb, " (closing)");
        sbprintf(sb, "\n");
    }
}

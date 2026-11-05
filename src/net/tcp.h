// TCP, the protocol engine: connections, their state machine, the
// buffers, retransmission and congestion control. It knows nothing of
// tasks, descriptors or locks; tcpsock.c puts it in the kernel, and
// tests/test_tcp.c runs it on the host against a simulated network.
// The caller serializes every call (the kernel holds net_lock).
//
// What it does: RFC 9293 with the MSS option and nothing else (no
// window scaling, timestamps or SACK, so a window is at most 64 KB),
// RFC 6298 retransmission timing, RFC 5681 congestion control with the
// NewReno fast recovery of RFC 6582, zero-window probes, delayed ACKs,
// out-of-order segments kept until the gap fills, and the RFC 5961
// checks that make a blind reset or SYN hard to inject.
#ifndef RINGTAIL_TCP_H
#define RINGTAIL_TCP_H

#include <stdint.h>

enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

#define TCP_HLEN 20
#define TCP_MSS 1460          // what we ask for: a full Ethernet frame
#define TCP_MSS_DEFAULT 536   // what a peer that names none takes (RFC 9293)
#define TCP_BUF 16384         // bytes buffered each way, per connection
#define TCP_MAX_CONNS 64      // including those closing in the background
#define TCP_BACKLOG_MAX 16
#define TCP_OOO 4             // gaps remembered in the receive buffer

#define TCP_RTO_INIT 1000     // ms, before there is a round trip to go by
#define TCP_RTO_MIN 200
#define TCP_RTO_MAX 60000
#define TCP_SYN_RETRIES 4     // SYNs at 0, 1, 3, 7 and 15 s; give up at 31
#define TCP_RETRIES 8         // then a connection is given up for lost
#define TCP_DELACK 40         // ms an ACK may wait for a second segment
#define TCP_TIME_WAIT_MS 30000
#define TCP_FIN_WAIT_2_MS 30000  // for the peer's FIN, once nobody holds the socket

// tcp_events: what a call would find. The values are poll()'s.
#define TCP_EV_IN  1   // tcp_recv would not wait (data, the end, or an error); tcp_accept has one
#define TCP_EV_OUT 4   // tcp_send would take TCP_BUF / 4 bytes at least
#define TCP_EV_ERR 8
#define TCP_EV_HUP 16  // nothing more will come either way

typedef struct { uint32_t start, end; } tcp_range_t;  // [start, end) in sequence space

typedef struct tcp {
    struct tcp* next;      // the table
    int state;
    uint32_t lip, rip;     // addresses, host order; lip is 0 for a listener
    uint16_t lport, rport;
    uint8_t mac[6];        // the next hop, for the platform's tcp_transmit
    int refs;              // descriptors, counted by the platform
    int err;               // why it ended: -ECONNRESET and so on, for the next call
    int used;              // has been connected (or tried to be): no second connect
    int orphan;            // nobody holds it; it finishes closing on its own
    int shut_rd, shut_wr;  // shut down for reading, and for writing (a FIN is due or sent)
    int fin_in;            // the peer's FIN came; reads end once the buffer is empty

    // A listener's connections: children counts those not yet
    // accepted, of which the established ones wait in accept_q.
    struct tcp* parent;    // for a child not yet accepted
    struct tcp* accept_q;
    struct tcp* accept_next;
    int backlog, children;

    // Sending. sbuf holds slen bytes from shead (a ring): those sent
    // and not yet acknowledged, then those not yet sent. The first of
    // them has sequence number sseq.
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, max_wnd;
    uint8_t* sbuf;
    uint32_t shead, slen, sseq;
    uint32_t mss;          // the most we put in one segment

    // Receiving. rbuf holds rlen bytes in order from rhead, then room,
    // where bytes that came early wait at their places (the ooo
    // ranges) for the gap before them to fill.
    uint32_t irs, rcv_nxt, rcv_adv;  // rcv_adv: the window's right edge, as last offered
    uint8_t* rbuf;
    uint32_t rhead, rlen;
    tcp_range_t ooo[TCP_OOO];
    int nooo;
    int fin_early;         // a FIN came after a gap, at fin_early_seq
    uint32_t fin_early_seq;

    // Timers: deadlines in ms, 0 when off.
    uint32_t rexmit_at, ack_at, linger_at;
    uint32_t rto, srtt, rttvar;  // ms; srtt 0 until the first sample
    int retries;
    int rtt_on;            // timing the segment that ends at rtt_seq, sent at rtt_at
    uint32_t rtt_seq, rtt_at;
    int ack_now, segs_unacked;
    int probe;             // send one byte into a closed window

    // Congestion control (RFC 5681, 6582).
    uint32_t cwnd, ssthresh, recover;
    int dupacks, recovering;
} tcp_t;

typedef struct {
    uint32_t in, out, bad;           // segments; bad: malformed or a bad checksum
    uint32_t retransmits, resets_in, resets_out;
    uint32_t active, passive;        // connections opened each way
    uint32_t syn_dropped;            // for a full backlog or table
    uint32_t challenge_acks;
} tcp_stats_t;
extern tcp_stats_t tcp_stats;

// What the platform provides.
uint32_t tcp_clock(void);            // ms, wrapping
uint32_t tcp_random(void);
void* tcp_alloc(uint32_t size);
void tcp_free(void* p);
uint8_t* tcp_txbuf(void);            // room for a segment (TCP_HLEN + 4 + TCP_MSS bytes)
// Send the len bytes in tcp_txbuf from src to dst, through the
// neighbour at mac (0 for a reply to something that came by loopback).
void tcp_transmit(const uint8_t* mac, uint32_t src, uint32_t dst, uint32_t len);
void tcp_wake(tcp_t* c);             // something about c a waiter cares about changed

// The calls. Errors are negative errno values.
tcp_t* tcp_new(void);                // unbound and CLOSED; 0 when out of memory or room
int tcp_bind(tcp_t* c, int port);    // the port; 0 picks a free one from 49152 up
int tcp_listen(tcp_t* c, int backlog);
// Start opening a connection: the SYN goes now, and c leaves
// TCP_SYN_SENT when it is answered, refused or given up on.
int tcp_connect(tcp_t* c, uint32_t lip, uint32_t rip, int rport, const uint8_t* mac);
tcp_t* tcp_accept(tcp_t* l);         // an established connection, now the caller's, or 0
int tcp_send(tcp_t* c, const uint8_t* buf, uint32_t len);  // bytes taken: 0 while the buffer is full
int tcp_recv(tcp_t* c, uint8_t* buf, uint32_t len);        // bytes, 0 at the end, -EAGAIN for none yet
int tcp_shutdown(tcp_t* c, int how); // 0 reading, 1 writing, 2 both
// The last descriptor went (refs is 0): finish in the background, or
// reset the connection if data it was sent will never be read.
void tcp_close(tcp_t* c);
void tcp_abort(tcp_t* c, int err);   // reset now; c stays, CLOSED, for its owner
int tcp_events(const tcp_t* c);

// A segment for us: src and dst are the IP addresses, mac the frame's
// sender (0 for loopback).
void tcp_input(uint32_t src, uint32_t dst, const uint8_t* seg, uint32_t len, const uint8_t* mac);
// An ICMP error about a segment we sent from src to dst, quoting at
// least the first 8 bytes of its header.
void tcp_icmp_error(uint32_t src, uint32_t dst, const uint8_t* th, int err);
// Run what is due. Returns ms until something next is, or ~0u.
uint32_t tcp_timers(void);
// The address went away: connections from it are over.
void tcp_addr_lost(uint32_t ip);
tcp_t* tcp_first(void);              // the table, for listing it
const char* tcp_state_name(int state);

#endif

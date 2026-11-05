// What the parts of the network stack share: net.c (Ethernet, ARP,
// IPv4, ICMP, loopback and the net task), udp.c (sockets) and dhcp.c
// (the DHCP client). Everything here is used with net_lock held.
#ifndef RINGTAIL_NETPRIV_H
#define RINGTAIL_NETPRIV_H

#include <stdint.h>
#include "net.h"
#include "sync.h"
#include "drivers.h"

#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806
#define IP_HLEN   20
#define UDP_HLEN  8
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_BROADCAST  0xFFFFFFFFu

extern netdev_t* net_dev;  // 0 without a card; loopback works anyway
extern mutex_t net_lock;

// The frame being built. A packet's payload goes at NET_PAYLOAD, after
// room for the Ethernet and IP headers that ip_send fills in.
extern uint8_t net_txbuf[ETH_FRAME_MAX];
#define NET_PAYLOAD (net_txbuf + ETH_HLEN + IP_HLEN)

// The address and what came with it. ip is 0 while there is none.
typedef struct {
    uint32_t ip, mask, gw, dns[2];
    char domain[64];
} ipconf_t;
extern ipconf_t net_cfg;

static inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// The Internet checksum (RFC 1071), in parts: sum the pieces with
// csum_add (each piece but the last of even length), then fold.
uint32_t csum_add(uint32_t sum, const uint8_t* p, uint32_t len);
uint16_t csum_fold(uint32_t sum);
static inline uint16_t inet_csum(const uint8_t* p, uint32_t len) { return csum_fold(csum_add(0, p, len)); }

// An address a host may have: not 0.x, loopback, multicast or above.
static inline int ip_unicast(uint32_t ip) {
    return (ip >> 24) != 0 && (ip >> 24) != 127 && (ip >> 28) < 14;
}
// The checks every sender makes before it takes net_lock: 0, or
// -ENETDOWN, or -EADDRNOTAVAIL.
int net_can_send(uint32_t ip);
int ip_local(uint32_t ip);          // 127.x.x.x, or our own address
int ip_broadcast(uint32_t ip);      // 255.255.255.255, or our network's broadcast
uint32_t ip_source(uint32_t dst);   // the source address for a packet to dst
// The neighbour a packet for dst goes to: dst itself on our network,
// else the gateway. 0, or -ENETUNREACH with no gateway for it.
int ip_route(uint32_t dst, uint32_t* hop);
// Send the len bytes at NET_PAYLOAD to dst, from src (0: ip_source),
// through the neighbour whose MAC is mac (ignored for loopback).
int ip_send(uint32_t src, uint32_t dst, const uint8_t* mac, uint8_t proto, uint32_t len);

// The MAC for a neighbour. arp_resolve asks and waits, dropping
// net_lock meanwhile, so never from the net task: arp_lookup asks and
// returns -EAGAIN instead, for the caller to try again later.
int arp_resolve(uint32_t ip, uint8_t* mac);
int arp_lookup(uint32_t ip, uint8_t* mac);
extern const uint8_t net_broadcast_mac[ETH_ALEN];

// Take on a new address (all zero for none). Forgets the neighbours if
// the network changed.
void net_set_config(const ipconf_t* c);
uint32_t net_random(void);
static inline uint64_t net_now(void) { return timer_ticks64(); }
void net_kick(void);  // wake the net task to look at its deadlines

// udp.c. udp_input takes a datagram addressed to us (iph is its IP
// header, for an ICMP error) and returns -1 if it is malformed.
int udp_input(const uint8_t* eth_src, const uint8_t* iph, uint32_t src, uint32_t dst, const uint8_t* p, uint32_t len);
// Fill in the UDP header at NET_PAYLOAD for len data bytes after it.
void udp_header(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t len);
void udp_format(sbuf_t* sb);
// ICMP "port unreachable" back to the sender of the packet at iph.
void icmp_port_unreachable(const uint8_t* eth_src, const uint8_t* iph, uint32_t len);

// dhcp.c, the client. dhcp_poll does whatever is due and returns when
// it next needs to run (net_now ticks, or ~0 for never).
void dhcp_start(void);
void dhcp_stop(void);
void dhcp_input(const uint8_t* p, uint32_t len);  // from port 67 to port 68
uint64_t dhcp_poll(uint64_t now);
void dhcp_conf(netconf_t* c);  // mode, lease and server
void dhcp_format(sbuf_t* sb);

#endif

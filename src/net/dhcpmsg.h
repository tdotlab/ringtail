// DHCP messages (RFC 2131 and 2132), built and read without touching
// anything else, so the host tests can feed the parser whatever they
// like.
#ifndef RINGTAIL_DHCPMSG_H
#define RINGTAIL_DHCPMSG_H

#include <stdint.h>

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7

#define DHCP_MIN_LEN 240   // the fixed part and the magic cookie
#define DHCP_BUILD_MAX 300 // what dhcp_build writes, at most (and at least: old servers want 300)

// What the client sends. requested and server become options 50 and
// 54 when not 0; broadcast sets the flag that asks the server to
// broadcast its answer, for a client that cannot take unicast yet.
typedef struct {
    int type;
    uint32_t xid;
    uint16_t secs;
    int broadcast;
    uint32_t ciaddr, requested, server;
    uint8_t mac[6];
} dhcp_req_t;

// What a server said. Fields whose option was missing are 0, and
// `have` has a bit for each option seen (DHCP_HAVE_*).
#define DHCP_HAVE_MASK  1
#define DHCP_HAVE_LEASE 2
#define DHCP_HAVE_T1    4
#define DHCP_HAVE_T2    8
typedef struct {
    int op, type;
    uint32_t xid, yiaddr, server;
    uint8_t chaddr[6];
    uint32_t mask, router, dns[2];
    uint32_t lease, t1, t2;  // seconds
    int have;
    char domain[64];         // "" unless a sensible one was given
} dhcp_msg_t;

// Writes the message to p (DHCP_BUILD_MAX bytes of room) and returns
// its length.
uint32_t dhcp_build(uint8_t* p, const dhcp_req_t* r);
// 0, or -1 for anything that is not a well-formed DHCP message from a
// server about an Ethernet address.
int dhcp_parse(const uint8_t* p, uint32_t len, dhcp_msg_t* m);

#endif

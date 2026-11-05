// The network stack: Ethernet, ARP, IPv4 and ICMP, over one card.
// Addresses are kept in host byte order (10.0.2.15 is 0x0A00020F)
// and converted only when a packet is read or built.
#ifndef RINGTAIL_NET_H
#define RINGTAIL_NET_H

#include <stdint.h>
#include "lib.h"
#include "ringtail/abi.h"

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_MTU 1500
#define ETH_FRAME_MAX (ETH_HLEN + ETH_MTU)

// What a driver provides. send copies the frame (destination MAC
// first, no FCS) and may be called with interrupts on or off; recv
// copies the next received frame into buf and returns its length, or
// 0 when none is waiting; rx_ready says whether recv would find one.
typedef struct netdev {
    const char* name;        // "e1000"
    const char* model;       // "82540EM"
    char where[12];          // PCI location, "00:03.0"
    uint16_t vendor, device;
    int irq;                 // -1 when it has none and must be polled
    uint8_t mac[ETH_ALEN];
    uint32_t rx_errors;      // frames the card received but threw away
    uint32_t interrupts;
    int (*send)(struct netdev* d, const void* frame, uint32_t len);
    int (*recv)(struct netdev* d, void* buf, uint32_t size);
    int (*rx_ready)(struct netdev* d);
    int (*link_up)(struct netdev* d);
} netdev_t;

// Find a card and start the stack's task. Prints one boot line.
void net_init(void);
// For drivers' interrupt handlers: frames have arrived.
void net_rx_notify(void);
// SYS_PING, with p already copied into the kernel.
int net_ping(ping_t* p);
void net_format(sbuf_t* sb);  // KINFO_NET

#define IPV4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

#endif

// The kernel's random numbers: unpredictable bytes for keys (TLS), and
// for the things an attacker would otherwise guess (TCP sequence
// numbers, ports, DNS and DHCP transaction ids).
#ifndef RINGTAIL_RANDOM_H
#define RINGTAIL_RANDOM_H

#include <stdint.h>

// Reads the first unpredictable things the machine has: the clock, the
// cycle counter, the CPU's own generator where there is one.
void random_init(void);

// Something happened whose exact moment nobody outside could know: an
// interrupt. Cheap; meant to be called from interrupt handlers.
void random_event(uint32_t what);

// len random bytes. May be called anywhere, interrupts on or off.
void random_bytes(void* buf, uint32_t len);

#endif

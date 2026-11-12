// SNTP messages (RFC 4330, on RFC 5905's format): a request built and a
// reply read, with no system calls, so the host tests can feed the
// parser whatever they like. sntp.c sends them.
//
// Times here are 64-bit fixed point, 32 bits of seconds and 32 of
// fraction. On the wire they count from 1900 and wrap every 2^32
// seconds (the next wrap is 2036-02-07 06:28:16 UTC); ntp_to_unix()
// picks the wrap nearest a time we already believe.
#ifndef RINGTAIL_NTPMSG_H
#define RINGTAIL_NTPMSG_H

#include <stdint.h>

#define NTP_PORT 123
#define NTP_SIZE 48                     // a message without extensions
#define NTP_UNIX_EPOCH 2208988800u      // 1970 in seconds since 1900
#define NTP_ONE ((int64_t)1 << 32)      // one second, in fixed point

// Errors, below the DNS ones so the two never meet.
#define NTP_E_NOTOURS  -1101  // not the answer to our request: ignore it
#define NTP_E_BADREPLY -1102  // our answer, but unusable (no timestamps, a version nobody speaks)
#define NTP_E_KISS     -1103  // kiss-o'-death: the server says go away (see kiss)
#define NTP_E_UNSYNC   -1104  // the server does not know the time itself
#define NTP_E_TIMEOUT  -1105
#define NTP_E_FAR      -1106  // a time more than 68 years from ours: no telling which era it is in

typedef struct {
    int leap;          // 0 to 2, a leap second coming (3 is refused as unsynchronised)
    int version;       // 1 to 4
    int stratum;       // 1 to 15
    uint32_t refid;
    char kiss[5];      // a kiss-o'-death's code, as sent (only for NTP_E_KISS)
    uint64_t recv;     // when the request arrived, by the server's clock (wire format)
    uint64_t xmit;     // when the reply left
} ntp_reply_t;

// A client request (version 4, mode 3) in p. Everything is zero but
// the first byte and the transmit timestamp, which is nonce: random,
// not the time, so the request says nothing about this machine's clock
// and a reply can only be ours if it echoes it (RFC 9109's advice).
void ntp_build_request(uint8_t p[NTP_SIZE], uint64_t nonce);
// Reads the len bytes at p as the reply to the request carrying nonce.
// 0 with r filled in; NTP_E_NOTOURS for anything that is not a server's
// reply to that request; NTP_E_KISS, NTP_E_UNSYNC or NTP_E_BADREPLY for
// one that is, but must not be used (r->kiss says which kiss).
int ntp_parse(const uint8_t* p, int len, uint64_t nonce, ntp_reply_t* r);
// A wire timestamp as Unix time in fixed point, in whichever 2^32 s
// era puts it within 68 years of pivot (Unix seconds). 0 if that is
// before 1970 or after 2106.
uint64_t ntp_to_unix(uint64_t t, uint32_t pivot);
// The clock's error and the round trip from the four times, all Unix
// fixed point: t1 the request left, t2 the server got it, t3 the reply
// left, t4 it came back. t1 and t4 are by our clock, t2 and t3 by the
// server's. offset is what to add to ours; delay is never negative.
void ntp_offset(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, int64_t* offset, int64_t* delay);

#endif

// SNTP messages: see ntpmsg.h.
//
// The reply's header (RFC 5905 7.3):
//   0  LI (2 bits), VN (3), mode (3)    1  stratum
//   2  poll                             3  precision
//   4  root delay                       8  root dispersion
//  12  reference ID                    16  reference timestamp
//  24  origin timestamp (our transmit) 32  receive timestamp
//  40  transmit timestamp
// Anything after byte 48 (extensions, a MAC) is ignored.
#include <stddef.h>
#include "ntpmsg.h"

static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

void ntp_build_request(uint8_t p[NTP_SIZE], uint64_t nonce) {
    for (int i = 0; i < NTP_SIZE; i++) p[i] = 0;
    p[0] = 4 << 3 | 3;  // no leap warning, version 4, client
    for (int i = 0; i < 8; i++) p[40 + i] = (uint8_t)(nonce >> (56 - 8 * i));
}

int ntp_parse(const uint8_t* p, int len, uint64_t nonce, ntp_reply_t* r) {
    if (len < NTP_SIZE || (p[0] & 7) != 4 || rd64(p + 24) != nonce) return NTP_E_NOTOURS;
    for (size_t i = 0; i < sizeof *r; i++) ((uint8_t*)r)[i] = 0;
    r->leap = p[0] >> 6;
    r->version = p[0] >> 3 & 7;
    r->stratum = p[1];
    r->refid = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
    r->recv = rd64(p + 32);
    r->xmit = rd64(p + 40);
    if (r->stratum == 0) {  // RFC 5905 7.4: the reference ID is the kiss code, in ASCII
        for (int i = 0; i < 4; i++) r->kiss[i] = (char)p[12 + i];
        return NTP_E_KISS;
    }
    if (r->version < 1 || r->version > 4 || !r->recv || !r->xmit) return NTP_E_BADREPLY;
    if (r->leap == 3 || r->stratum > 15) return NTP_E_UNSYNC;
    return 0;
}

uint64_t ntp_to_unix(uint64_t t, uint32_t pivot) {
    uint32_t secs = (uint32_t)(t >> 32);
    int64_t u = (int64_t)pivot + (int32_t)(secs - (pivot + NTP_UNIX_EPOCH));
    if (u < 0 || u > 0xFFFFFFFFll) return 0;
    return (uint64_t)u << 32 | (uint32_t)t;
}

void ntp_offset(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4, int64_t* offset, int64_t* delay) {
    // ntp_to_unix put the server's times within 68 years (2^31 s) of
    // ours, so each difference fits in 63 bits; their sum might not, so
    // halve first. A right shift of a negative value is arithmetic in gcc.
    int64_t a = (int64_t)(t2 - t1), b = (int64_t)(t3 - t4);
    *offset = (a >> 1) + (b >> 1);
    int64_t d = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
    *delay = d < 0 ? 0 : d;
}

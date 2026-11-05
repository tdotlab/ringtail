// DHCP messages: see dhcpmsg.h.
#include "dhcpmsg.h"
#include "lib.h"

#define OPT_PAD      0
#define OPT_MASK     1
#define OPT_ROUTER   3
#define OPT_DNS      6
#define OPT_HOSTNAME 12
#define OPT_DOMAIN   15
#define OPT_REQUESTED 50
#define OPT_LEASE    51
#define OPT_TYPE     53
#define OPT_SERVER   54
#define OPT_PARAMS   55
#define OPT_MAXSIZE  57
#define OPT_T1       58
#define OPT_T2       59
#define OPT_CLIENTID 61
#define OPT_END      255

static const uint8_t cookie[4] = { 99, 130, 83, 99 };

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

uint32_t dhcp_build(uint8_t* p, const dhcp_req_t* r) {
    memset(p, 0, DHCP_BUILD_MAX);
    p[0] = 1;  // a request, from a client
    p[1] = 1;  // Ethernet
    p[2] = 6;
    wr32(p + 4, r->xid);
    p[8] = r->secs >> 8;
    p[9] = r->secs;
    if (r->broadcast) p[10] = 0x80;
    wr32(p + 12, r->ciaddr);
    memcpy(p + 28, r->mac, 6);
    memcpy(p + 236, cookie, 4);
    uint8_t* o = p + 240;
    *o++ = OPT_TYPE; *o++ = 1; *o++ = (uint8_t)r->type;
    *o++ = OPT_CLIENTID; *o++ = 7; *o++ = 1; memcpy(o, r->mac, 6); o += 6;
    if (r->requested) { *o++ = OPT_REQUESTED; *o++ = 4; wr32(o, r->requested); o += 4; }
    if (r->server) { *o++ = OPT_SERVER; *o++ = 4; wr32(o, r->server); o += 4; }
    if (r->type == DHCPDISCOVER || r->type == DHCPREQUEST) {
        static const uint8_t params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_DOMAIN, OPT_LEASE, OPT_T1, OPT_T2 };
        *o++ = OPT_PARAMS; *o++ = sizeof params; memcpy(o, params, sizeof params); o += sizeof params;
        *o++ = OPT_MAXSIZE; *o++ = 2; *o++ = 1500 >> 8; *o++ = 1500 & 255;
        *o++ = OPT_HOSTNAME; *o++ = 8; memcpy(o, "ringtail", 8); o += 8;
    }
    *o++ = OPT_END;
    return DHCP_BUILD_MAX;  // padded with zeros, which are pad options
}

// Letters, digits, '-', '_' and dots between labels, as a DNS search
// domain must be. A trailing dot is allowed and dropped.
static int domain_ok(const char* d, uint32_t n) {
    if (n == 0 || d[0] == '.') return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = d[i];
        if (c == '.' && i + 1 < n && d[i + 1] == '.') return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

int dhcp_parse(const uint8_t* p, uint32_t len, dhcp_msg_t* m) {
    memset(m, 0, sizeof *m);
    if (len < DHCP_MIN_LEN || p[0] != 2 || p[1] != 1 || p[2] != 6 || memcmp(p + 236, cookie, 4)) return -1;
    m->op = p[0];
    m->xid = rd32(p + 4);
    m->yiaddr = rd32(p + 16);
    memcpy(m->chaddr, p + 28, 6);
    for (uint32_t i = DHCP_MIN_LEN; i < len;) {
        uint8_t code = p[i];
        if (code == OPT_PAD) { i++; continue; }
        if (code == OPT_END) break;
        if (i + 2 > len) return -1;
        uint32_t n = p[i + 1];
        const uint8_t* v = p + i + 2;
        if (i + 2 + n > len) return -1;  // runs off the end
        i += 2 + n;
        switch (code) {
        case OPT_TYPE: if (n == 1) m->type = v[0]; break;
        case OPT_SERVER: if (n == 4) m->server = rd32(v); break;
        case OPT_MASK: if (n == 4) { m->mask = rd32(v); m->have |= DHCP_HAVE_MASK; } break;
        case OPT_ROUTER: if (n >= 4) m->router = rd32(v); break;
        case OPT_DNS:
            m->dns[0] = m->dns[1] = 0;
            if (n >= 4) m->dns[0] = rd32(v);
            if (n >= 8) m->dns[1] = rd32(v + 4);
            break;
        case OPT_LEASE: if (n == 4) { m->lease = rd32(v); m->have |= DHCP_HAVE_LEASE; } break;
        case OPT_T1: if (n == 4) { m->t1 = rd32(v); m->have |= DHCP_HAVE_T1; } break;
        case OPT_T2: if (n == 4) { m->t2 = rd32(v); m->have |= DHCP_HAVE_T2; } break;
        case OPT_DOMAIN: {
            // Some servers count a trailing NUL or two in the length.
            uint32_t k = n;
            while (k && v[k - 1] == 0) k--;
            if (k && v[k - 1] == '.') k--;
            m->domain[0] = 0;
            if (k < sizeof m->domain && domain_ok((const char*)v, k)) {
                memcpy(m->domain, v, k);
                m->domain[k] = 0;
            }
            break;
        }
        }
    }
    return m->type ? 0 : -1;
}

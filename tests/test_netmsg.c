// Host tests for the pieces of the network stack that read what
// strangers send: the DHCP messages (src/net/dhcpmsg.c), the DNS
// messages (src/programs/lib/dnsmsg.c) and the SNTP messages
// (src/programs/lib/ntpmsg.c), with the calendar arithmetic the clock
// is set through (src/lib/civil.c). Built with the address
// and undefined-behaviour sanitizers, so a read past the end of a
// packet is a failure even when it happens to return a harmless byte.
// Each parser also gets a few hundred thousand mangled messages.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "dhcpmsg.h"
#include "dnsmsg.h"
#include "ntpmsg.h"
#include "civil.h"

static int passed, failed;
#define CHECK(cond, what) do { \
    if (cond) passed++; \
    else { failed++; printf("  FAIL  %s (line %d)\n", what, __LINE__); } \
} while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// ---- DHCP ----

static const uint8_t mac[6] = { 0x52, 0x54, 0, 0x12, 0x34, 0x56 };

// The options area of a message built by dhcp_build: the code's value
// and its length, or 0 if it is not there.
static const uint8_t* find_opt(const uint8_t* p, int code, int* len) {
    for (int i = 240; i < DHCP_BUILD_MAX && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        if (p[i] == code) { *len = p[i + 1]; return p + i + 2; }
        i += 2 + p[i + 1];
    }
    return 0;
}

static void test_dhcp_build(void) {
    uint8_t p[DHCP_BUILD_MAX];
    dhcp_req_t r = { DHCPDISCOVER, 0xA1B2C3D4, 7, 1, 0, 0x0A00020F, 0, { 0 } };
    memcpy(r.mac, mac, 6);
    memset(p, 0xEE, sizeof p);
    CHECK(dhcp_build(p, &r) == 300, "a message is 300 bytes, as old servers want");
    CHECK(p[0] == 1 && p[1] == 1 && p[2] == 6 && p[3] == 0, "op, htype, hlen and hops");
    CHECK(rd32(p + 4) == 0xA1B2C3D4 && p[8] == 0 && p[9] == 7, "xid and secs");
    CHECK(p[10] == 0x80 && p[11] == 0, "the broadcast flag");
    CHECK(rd32(p + 12) == 0 && rd32(p + 16) == 0 && rd32(p + 20) == 0 && rd32(p + 24) == 0, "no addresses yet");
    CHECK(!memcmp(p + 28, mac, 6) && p[34] == 0, "chaddr");
    CHECK(p[236] == 99 && p[237] == 130 && p[238] == 83 && p[239] == 99, "the magic cookie");
    int n;
    const uint8_t* o = find_opt(p, 53, &n);
    CHECK(o && n == 1 && o[0] == DHCPDISCOVER, "option 53, the message type");
    o = find_opt(p, 61, &n);
    CHECK(o && n == 7 && o[0] == 1 && !memcmp(o + 1, mac, 6), "option 61, the client id");
    o = find_opt(p, 50, &n);
    CHECK(o && n == 4 && rd32(o) == 0x0A00020F, "option 50, the address wanted");
    CHECK(!find_opt(p, 54, &n), "no server id in a DISCOVER");
    o = find_opt(p, 55, &n);
    CHECK(o && n == 8 && o[0] == 1 && o[1] == 3 && o[2] == 6, "option 55 asks for mask, router, DNS...");
    CHECK(o && memchr(o, 42, (size_t)n), "...and time servers (42)");
    o = find_opt(p, 57, &n);
    CHECK(o && n == 2 && o[0] == 5 && o[1] == 220, "option 57: 1500 bytes will do");
    o = find_opt(p, 12, &n);
    CHECK(o && n == 8 && !memcmp(o, "ringtail", 8), "option 12, the host name");
    int end = 240;
    while (p[end] != 255) end += p[end] ? 2 + p[end + 1] : 1;
    int zeros = 1;
    for (int i = end + 1; i < 300; i++) zeros &= p[i] == 0;
    CHECK(zeros, "padded with zeros after the end option");

    dhcp_req_t q = { DHCPREQUEST, 1, 0, 0, 0x0A00020F, 0x0A00020F, 0x0A000202, { 0 } };
    dhcp_build(p, &q);
    CHECK(p[10] == 0 && rd32(p + 12) == 0x0A00020F, "a renewal: no broadcast flag, ciaddr filled in");
    o = find_opt(p, 54, &n);
    CHECK(o && n == 4 && rd32(o) == 0x0A000202, "option 54, the server");
    dhcp_req_t z = { DHCPREQUEST, 1, 0, 0, 0, 0, 0, { 0 } };
    dhcp_build(p, &z);
    CHECK(!find_opt(p, 50, &n) && !find_opt(p, 54, &n), "options 50 and 54 only when there is something to say");
}

// A server's reply, with the options given as code, length, bytes...
static int server_msg(uint8_t* p, uint32_t xid, uint32_t yiaddr, const uint8_t* opts, int olen) {
    memset(p, 0, 576);
    p[0] = 2; p[1] = 1; p[2] = 6;
    wr32(p + 4, xid);
    wr32(p + 16, yiaddr);
    memcpy(p + 28, mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    memcpy(p + 240, opts, (size_t)olen);
    return 240 + olen;
}

static void test_dhcp_parse(void) {
    uint8_t p[576];
    dhcp_msg_t m;
    static const uint8_t offer[] = {
        53, 1, 2, 54, 4, 10, 0, 2, 2, 1, 4, 255, 255, 255, 0, 3, 8, 10, 0, 2, 2, 10, 0, 2, 1,
        6, 12, 10, 0, 2, 3, 8, 8, 8, 8, 1, 1, 1, 1, 51, 4, 0, 1, 81, 128, 58, 4, 0, 0, 0, 100,
        59, 4, 0, 0, 0, 200, 15, 13, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', '.', 0,
        42, 12, 10, 0, 2, 4, 10, 0, 2, 5, 10, 0, 2, 6, 255,
    };
    int len = server_msg(p, 77, 0x0A00020F, offer, sizeof offer);
    CHECK(dhcp_parse(p, (uint32_t)len, &m) == 0, "an offer parses");
    CHECK(m.type == DHCPOFFER && m.xid == 77 && m.yiaddr == 0x0A00020F && !memcmp(m.chaddr, mac, 6), "type, xid, yiaddr, chaddr");
    CHECK(m.server == 0x0A000202 && m.mask == 0xFFFFFF00 && m.router == 0x0A000202, "server, mask, the first router");
    CHECK(m.dns[0] == 0x0A000203 && m.dns[1] == 0x08080808, "the first two DNS servers");
    CHECK(m.lease == 86400 && m.t1 == 100 && m.t2 == 200, "lease, T1 and T2");
    CHECK(m.have == (DHCP_HAVE_MASK | DHCP_HAVE_LEASE | DHCP_HAVE_T1 | DHCP_HAVE_T2), "and which of those came");
    CHECK(!strcmp(m.domain, "example.com"), "the domain, without its trailing dot and NUL");
    CHECK(m.ntp[0] == 0x0A000204 && m.ntp[1] == 0x0A000205, "the first two time servers");

    CHECK(dhcp_parse(p, 239, &m) == -1, "too short for the cookie");
    p[0] = 1;
    CHECK(dhcp_parse(p, (uint32_t)len, &m) == -1, "a client's message is not a server's");
    p[0] = 2; p[236] = 0;
    CHECK(dhcp_parse(p, (uint32_t)len, &m) == -1, "no cookie, no DHCP");
    p[236] = 99; p[2] = 16;
    CHECK(dhcp_parse(p, (uint32_t)len, &m) == -1, "only 6-byte Ethernet addresses");
    p[2] = 6;

    static const uint8_t no_type[] = { 1, 4, 255, 255, 0, 0, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, no_type, sizeof no_type), &m) == -1, "BOOTP without a type is ignored");
    static const uint8_t runs_off[] = { 53, 1, 5, 6, 40, 1, 2, 3 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, runs_off, sizeof runs_off), &m) == -1, "an option running off the end");
    static const uint8_t no_end[] = { 53, 1, 5, 0, 0, 0 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, no_end, sizeof no_end), &m) == 0 && m.type == DHCPACK,
          "no end option: the packet's end will do");
    static const uint8_t odd_sizes[] = { 53, 1, 5, 1, 3, 1, 2, 3, 51, 2, 0, 9, 54, 5, 1, 2, 3, 4, 5, 6, 3, 1, 2, 3, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, odd_sizes, sizeof odd_sizes), &m) == 0 &&
          !(m.have & (DHCP_HAVE_MASK | DHCP_HAVE_LEASE)) && m.server == 0 && m.dns[0] == 0, "options of the wrong size are skipped");
    static const uint8_t bad_domains[] = { 53, 1, 5, 15, 6, 'a', '.', '.', 'b', 'c', 'd', 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, bad_domains, sizeof bad_domains), &m) == 0 && !m.domain[0], "a domain with an empty label");
    static const uint8_t esc_domain[] = { 53, 1, 5, 15, 5, 'a', 27, '[', '2', 'J', 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, esc_domain, sizeof esc_domain), &m) == 0 && !m.domain[0],
          "a domain with an escape sequence in it");
    uint8_t long_domain[80] = { 53, 1, 5, 15, 70 };
    memset(long_domain + 5, 'x', 70);
    long_domain[75] = 255;
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, long_domain, 76), &m) == 0 && !m.domain[0], "a domain too long to keep");
    static const uint8_t two_dns[] = { 53, 1, 5, 6, 4, 1, 1, 1, 1, 6, 4, 9, 9, 9, 9, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, two_dns, sizeof two_dns), &m) == 0 &&
          m.dns[0] == 0x09090909 && m.dns[1] == 0, "a repeated option: the last one counts");
    static const uint8_t ntp_odd[] = { 53, 1, 5, 42, 3, 1, 2, 3, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, ntp_odd, sizeof ntp_odd), &m) == 0 && m.ntp[0] == 0,
          "a time server option too short for an address");
    static const uint8_t ntp_two[] = { 53, 1, 5, 42, 8, 1, 1, 1, 1, 2, 2, 2, 2, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, ntp_two, sizeof ntp_two), &m) == 0 &&
          m.ntp[0] == 0x01010101 && m.ntp[1] == 0x02020202, "exactly two time servers");
    static const uint8_t ntp_again[] = { 53, 1, 5, 42, 8, 1, 1, 1, 1, 2, 2, 2, 2, 42, 4, 9, 9, 9, 9, 255 };
    CHECK(dhcp_parse(p, (uint32_t)server_msg(p, 1, 1, ntp_again, sizeof ntp_again), &m) == 0 &&
          m.ntp[0] == 0x09090909 && m.ntp[1] == 0, "time servers given twice: the last option counts, all of it");
}

static void fuzz_dhcp(void) {
    static const uint8_t base[] = {
        53, 1, 5, 54, 4, 10, 0, 2, 2, 1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 2, 2, 6, 8, 10, 0, 2, 3, 8, 8, 8, 8,
        51, 4, 0, 1, 81, 128, 15, 7, 'a', '.', 'b', '-', 'c', '.', 0, 42, 8, 10, 0, 2, 4, 10, 0, 2, 5, 255,
    };
    uint8_t p[576];
    int ok = 1;
    for (int i = 0; i < 200000; i++) {
        int len = server_msg(p, 5, 0x0A00020F, base, sizeof base);
        for (int k = 1 + (int)(rnd() % 8); k > 0; k--) {
            int at = 236 + (int)(rnd() % (uint32_t)(len - 236 + 4));
            if (at >= 576) at = 575;
            p[at] = (uint8_t)(rnd() % 3 ? rnd() : rnd() % 4 == 0 ? 255 : 0);
        }
        if (rnd() % 4 == 0) len = 236 + (int)(rnd() % (uint32_t)(len - 236 + 1));
        if (rnd() % 8 == 0) len += (int)(rnd() % 20);
        if (len > 576) len = 576;
        // A copy of exactly len bytes, so the sanitizer sees any read past it.
        uint8_t* q = (uint8_t*)malloc((size_t)len);
        memcpy(q, p, (size_t)len);
        dhcp_msg_t m;
        if (dhcp_parse(q, (uint32_t)len, &m) == 0)
            ok &= m.type != 0 && strlen(m.domain) < sizeof m.domain;
        free(q);
    }
    CHECK(ok, "200000 mangled DHCP messages: no crash, nothing out of bounds");
}

// ---- DNS ----

// Builds a DNS reply: a header, then names and records appended as
// they come. Names are written as "a.b.c", or "\xC0\x0C" style pointers
// by name_ptr.
typedef struct { uint8_t b[1500]; int n; } msg_t;

static void hdr(msg_t* m, uint16_t id, int flags, int qd, int an) {
    memset(m, 0, sizeof *m);
    wr16(m->b, id);
    wr16(m->b + 2, (uint32_t)flags);
    wr16(m->b + 4, (uint32_t)qd);
    wr16(m->b + 6, (uint32_t)an);
    m->n = 12;
}

// The labels of name, then a pointer to off if off >= 0, else the root.
static int name_at(msg_t* m, const char* name, int off) {
    int at = m->n;
    while (*name) {
        const char* e = strchr(name, '.');
        int len = e ? (int)(e - name) : (int)strlen(name);
        m->b[m->n++] = (uint8_t)len;
        memcpy(m->b + m->n, name, (size_t)len);
        m->n += len;
        name += len + (e ? 1 : 0);
    }
    if (off >= 0) { m->b[m->n++] = (uint8_t)(0xC0 | off >> 8); m->b[m->n++] = (uint8_t)off; }
    else m->b[m->n++] = 0;
    return at;
}

static void u16(msg_t* m, uint32_t v) { wr16(m->b + m->n, v); m->n += 2; }
static void u32(msg_t* m, uint32_t v) { wr32(m->b + m->n, v); m->n += 4; }

static int question(msg_t* m, const char* name, int type) {
    int at = name_at(m, name, -1);
    u16(m, (uint32_t)type);
    u16(m, 1);
    return at;
}

// A record whose owner is at owner (a pointer), class IN.
static void rr_head(msg_t* m, int owner, int type, int rdlen) {
    m->b[m->n++] = (uint8_t)(0xC0 | owner >> 8);
    m->b[m->n++] = (uint8_t)owner;
    u16(m, (uint32_t)type);
    u16(m, 1);
    u32(m, 300);
    u16(m, (uint32_t)rdlen);
}

static void rr_a(msg_t* m, int owner, uint32_t ip) { rr_head(m, owner, DNS_TYPE_A, 4); u32(m, ip); }

// A CNAME or PTR record; returns where its target name is.
static int rr_name(msg_t* m, int owner, int type, const char* target, int ptr) {
    rr_head(m, owner, type, 0);
    int len_at = m->n - 2, at = name_at(m, target, ptr);
    wr16(m->b + len_at, (uint32_t)(m->n - at));
    return at;
}

#define REPLY 0x8180  // QR, RD, RA

static void test_dns_names(void) {
    CHECK(dns_name_ok("example.com") && dns_name_ok("example.com.") && dns_name_ok("a") && dns_name_ok("_srv-x.A9"),
          "ordinary names");
    CHECK(!dns_name_ok("") && !dns_name_ok(".") && !dns_name_ok("..") && !dns_name_ok(".a") && !dns_name_ok("a..b") &&
          !dns_name_ok("a.b.."), "empty labels");
    CHECK(!dns_name_ok("a b") && !dns_name_ok("a\x1b[2J") && !dns_name_ok("caf\xc3\xa9") && !dns_name_ok("a/b"),
          "characters a name cannot have");
    char label[80];
    memset(label, 'x', 63); label[63] = 0;
    CHECK(dns_name_ok(label), "a 63-byte label");
    label[63] = 'x'; label[64] = 0;
    CHECK(!dns_name_ok(label), "a 64-byte one");
    char name[300];
    memset(name, 0, sizeof name);
    for (int i = 0; i < 253; i++) name[i] = i % 2 ? '.' : 'a';  // a.a.a...a, 253 long
    CHECK(dns_name_ok(name), "253 characters");
    name[253] = '.';
    CHECK(dns_name_ok(name), "253 and a trailing dot");
    name[253] = 'b';
    CHECK(!dns_name_ok(name), "254 is too long");
    CHECK(dns_name_eq("Example.COM", "example.com.") && dns_name_eq(".", ".") && !dns_name_eq("example.co", "example.com") &&
          !dns_name_eq("a.b", "ab"), "names compare without case or the trailing dot");

    uint8_t q[DNS_QUERY_MAX];
    int n = dns_build_query(q, sizeof q, 0xBEEF, "www.Example.com.", DNS_TYPE_A);
    static const uint8_t want[] = { 0xBE, 0xEF, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'w', 'w', 'w', 7, 'E', 'x', 'a', 'm', 'p',
                                    'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    CHECK(n == (int)sizeof want && !memcmp(q, want, sizeof want), "a query, byte for byte");
    CHECK(dns_build_query(q, sizeof q, 1, "a..b", DNS_TYPE_A) == DNS_E_BADNAME, "a bad name builds nothing");
    CHECK(dns_build_query(q, DNS_QUERY_MAX - 1, 1, "a", DNS_TYPE_A) == DNS_E_BADNAME, "nor into too small a buffer");
    for (int i = 0; i < 253; i++) name[i] = i % 64 == 63 ? '.' : 'z';  // four long labels
    name[253] = 0;
    n = dns_build_query(q, sizeof q, 1, name, DNS_TYPE_PTR);
    CHECK(n == 12 + 255 + 4 && n <= DNS_QUERY_MAX, "the longest name fits in DNS_QUERY_MAX");
}

static void test_dns_parse(void) {
    msg_t m;
    dns_answer_t a;

    hdr(&m, 7, REPLY, 1, 2);
    int q = question(&m, "example.com", DNS_TYPE_A);
    rr_a(&m, q, 0x5DB8D822);
    rr_a(&m, q, 0x01020304);
    CHECK(dns_parse(m.b, m.n, 7, "example.com", DNS_TYPE_A, &a) == 0, "an answer");
    CHECK(a.rcode == 0 && a.naddrs == 2 && a.addrs[0] == 0x5DB8D822 && a.addrs[1] == 0x01020304 && a.nchain == 0,
          "with both addresses, in order");
    CHECK(dns_parse(m.b, m.n, 7, "EXAMPLE.com.", DNS_TYPE_A, &a) == 0, "the question matches without case");
    CHECK(dns_parse(m.b, m.n, 8, "example.com", DNS_TYPE_A, &a) == DNS_E_NOTOURS, "another ID is not ours");
    CHECK(dns_parse(m.b, m.n, 7, "example.org", DNS_TYPE_A, &a) == DNS_E_NOTOURS, "nor another name");
    CHECK(dns_parse(m.b, m.n, 7, "example.com", DNS_TYPE_PTR, &a) == DNS_E_NOTOURS, "nor another type");
    m.b[2] &= 0x7F;
    CHECK(dns_parse(m.b, m.n, 7, "example.com", DNS_TYPE_A, &a) == DNS_E_NOTOURS, "nor a query");
    m.b[2] |= 0x80 | 0x28;
    CHECK(dns_parse(m.b, m.n, 7, "example.com", DNS_TYPE_A, &a) == DNS_E_NOTOURS, "nor another opcode");
    m.b[2] = 0x81;
    CHECK(dns_parse(m.b, 11, 7, "example.com", DNS_TYPE_A, &a) == DNS_E_NOTOURS, "nor 11 bytes");
    CHECK(dns_parse(m.b, m.n - 1, 7, "example.com", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "a record cut short is bad");
    m.b[m.n - 5] = 5;  // the second record's rdlength
    CHECK(dns_parse(m.b, m.n, 7, "example.com", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "an A record of 5 bytes is bad");

    hdr(&m, 7, REPLY, 1, 3);
    q = question(&m, "x.org", DNS_TYPE_A);
    rr_a(&m, q, 9);
    rr_a(&m, q, 9);
    rr_head(&m, q, DNS_TYPE_A, 4); m.b[m.n - 8] = 0; m.b[m.n - 7] = 3; u32(&m, 10);  // class CH
    CHECK(dns_parse(m.b, m.n, 7, "x.org", DNS_TYPE_A, &a) == 0 && a.naddrs == 1 && a.addrs[0] == 9,
          "repeats and other classes are dropped");

    hdr(&m, 7, REPLY, 1, 9);
    q = question(&m, "many.org", DNS_TYPE_A);
    for (int i = 1; i <= 9; i++) rr_a(&m, q, (uint32_t)i);
    CHECK(dns_parse(m.b, m.n, 7, "many.org", DNS_TYPE_A, &a) == 0 && a.naddrs == DNS_MAX_ADDRS, "at most 8 addresses kept");

    // www.example.com -> web.example.com -> edge.cdn.net, which has the address;
    // an address for a name nobody asked about is ignored.
    hdr(&m, 9, REPLY, 1, 4);
    q = question(&m, "www.example.com", DNS_TYPE_A);
    int web = rr_name(&m, q, DNS_TYPE_CNAME, "web", q + 4);
    int edge = rr_name(&m, web, DNS_TYPE_CNAME, "edge.cdn.net", -1);
    rr_a(&m, q + 4, 0x0B0B0B0B);  // example.com itself
    rr_a(&m, edge, 0x0C0C0C0C);
    CHECK(dns_parse(m.b, m.n, 9, "www.example.com", DNS_TYPE_A, &a) == 0, "a chain of aliases");
    CHECK(a.nchain == 2 && !strcmp(a.chain[0], "web.example.com") && !strcmp(a.chain[1], "edge.cdn.net"),
          "followed through compression");
    CHECK(a.naddrs == 1 && a.addrs[0] == 0x0C0C0C0C, "to the end's address only");

    hdr(&m, 9, REPLY, 1, 1);
    q = question(&m, "alias.org", DNS_TYPE_A);
    rr_name(&m, q, DNS_TYPE_CNAME, "real.org", -1);
    CHECK(dns_parse(m.b, m.n, 9, "alias.org", DNS_TYPE_A, &a) == 0 && a.nchain == 1 && a.naddrs == 0,
          "an alias with nothing after it, for the resolver to follow");

    hdr(&m, 9, REPLY, 1, 2);
    q = question(&m, "a.org", DNS_TYPE_A);
    int b = rr_name(&m, q, DNS_TYPE_CNAME, "b.org", -1);
    rr_name(&m, b, DNS_TYPE_CNAME, "a.org", -1);
    CHECK(dns_parse(m.b, m.n, 9, "a.org", DNS_TYPE_A, &a) == DNS_E_LOOP, "aliases in a circle");

    hdr(&m, 3, REPLY | DNS_RCODE_NXDOMAIN, 1, 0);
    question(&m, "nope.invalid", DNS_TYPE_A);
    CHECK(dns_parse(m.b, m.n, 3, "nope.invalid", DNS_TYPE_A, &a) == 0 && a.rcode == DNS_RCODE_NXDOMAIN, "NXDOMAIN");
    hdr(&m, 3, REPLY | DNS_RCODE_SERVFAIL, 0, 0);
    CHECK(dns_parse(m.b, m.n, 3, "x", DNS_TYPE_A, &a) == 0 && a.rcode == DNS_RCODE_SERVFAIL, "an error without the question");
    hdr(&m, 3, REPLY, 0, 0);
    CHECK(dns_parse(m.b, m.n, 3, "x", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "but not a success without it");
    hdr(&m, 3, REPLY, 2, 0);
    question(&m, "x", DNS_TYPE_A);
    question(&m, "x", DNS_TYPE_A);
    CHECK(dns_parse(m.b, m.n, 3, "x", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "two questions");

    // Truncated: the records that came whole still count.
    hdr(&m, 4, REPLY | 0x0200, 1, 3);
    q = question(&m, "big.org", DNS_TYPE_A);
    rr_a(&m, q, 1);
    rr_a(&m, q, 2);
    rr_a(&m, q, 3);
    CHECK(dns_parse(m.b, m.n - 6, 4, "big.org", DNS_TYPE_A, &a) == 0 && a.truncated && a.naddrs == 2,
          "a truncated answer keeps its whole records");

    // PTR, with bytes in the name that a terminal should not see raw.
    hdr(&m, 5, REPLY, 1, 1);
    q = question(&m, "4.3.2.1.in-addr.arpa", DNS_TYPE_PTR);
    rr_name(&m, q, DNS_TYPE_PTR, "h\x1b" "b.a\\Zx", -1);
    *(uint8_t*)memchr(m.b, 'Z', (size_t)m.n) = '.';  // a dot inside a label
    CHECK(dns_parse(m.b, m.n, 5, "4.3.2.1.in-addr.arpa", DNS_TYPE_PTR, &a) == 0 && !strcmp(a.ptr, "h\\027b.a\\\\\\.x"),
          "a PTR name, odd bytes escaped");

    // Compression that loops or points forward.
    hdr(&m, 6, REPLY, 1, 1);
    q = question(&m, "l.org", DNS_TYPE_A);
    int self = m.n;
    m.b[m.n++] = 0xC0; m.b[m.n++] = (uint8_t)self;  // the owner points at itself
    u16(&m, 1); u16(&m, 1); u32(&m, 1); u16(&m, 4); u32(&m, 1);
    CHECK(dns_parse(m.b, m.n, 6, "l.org", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "a pointer to itself");
    m.b[self + 1] = (uint8_t)(self + 2);
    CHECK(dns_parse(m.b, m.n, 6, "l.org", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "a pointer forward");
    hdr(&m, 6, REPLY, 1, 1);
    q = question(&m, "l.org", DNS_TYPE_A);
    int lab = m.n;
    m.b[m.n++] = 1; m.b[m.n++] = 'x';
    m.b[m.n++] = 0xC0; m.b[m.n++] = (uint8_t)lab;  // x.x.x.x... for ever
    u16(&m, 1); u16(&m, 1); u32(&m, 1); u16(&m, 4); u32(&m, 1);
    CHECK(dns_parse(m.b, m.n, 6, "l.org", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "a pointer back into its own labels");
    m.b[lab] = 0x40;
    CHECK(dns_parse(m.b, m.n, 6, "l.org", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "label type 01");

    // A name that only gets too long through its pointers.
    hdr(&m, 6, REPLY, 1, 1);
    q = question(&m, "l.org", DNS_TYPE_A);
    int prev = -1;
    for (int i = 0; i < 5; i++) {
        int at = m.n;
        char l[64];
        memset(l, 'a' + i, 60); l[60] = 0;
        name_at(&m, l, prev);
        prev = at;
    }
    rr_name(&m, q, DNS_TYPE_CNAME, "z", prev);
    CHECK(dns_parse(m.b, m.n, 6, "l.org", DNS_TYPE_A, &a) == DNS_E_BADREPLY, "a 300-byte name made of pointers");
}

static void fuzz_dns(void) {
    msg_t base;
    hdr(&base, 0x1234, REPLY, 1, 4);
    int q = question(&base, "www.example.com", DNS_TYPE_A);
    int web = rr_name(&base, q, DNS_TYPE_CNAME, "web", q + 4);
    rr_a(&base, web, 0x0A000001);
    rr_a(&base, web, 0x0A000002);
    rr_name(&base, q + 4, DNS_TYPE_PTR, "p", web);
    int ok = 1, parsed = 0;
    for (int i = 0; i < 300000; i++) {
        msg_t m = base;
        for (int k = 1 + (int)(rnd() % 6); k > 0; k--) {
            int at = (int)(rnd() % (uint32_t)m.n);
            switch (rnd() % 4) {
            case 0: m.b[at] = (uint8_t)rnd(); break;
            case 1: m.b[at] = (uint8_t)(0xC0 | (rnd() & 1)); if (at + 1 < m.n) m.b[at + 1] = (uint8_t)(rnd() % (uint32_t)m.n); break;
            case 2: m.b[at] ^= (uint8_t)(1 << (rnd() % 8)); break;
            default: m.b[at] = (uint8_t)(rnd() % 64); break;
            }
        }
        int len = m.n;
        if (rnd() % 4 == 0) len = (int)(rnd() % (uint32_t)(m.n + 1));
        uint8_t* p = (uint8_t*)malloc((size_t)len + 1);
        memcpy(p, m.b, (size_t)len);
        dns_answer_t a;
        int r = dns_parse(p, len, 0x1234, "www.example.com", rnd() % 4 ? DNS_TYPE_A : DNS_TYPE_PTR, &a);
        if (r == 0) {
            parsed++;
            ok &= a.naddrs >= 0 && a.naddrs <= DNS_MAX_ADDRS && a.nchain >= 0 && a.nchain <= DNS_MAX_CHAIN &&
                  strlen(a.ptr) < DNS_NAME_SIZE;
            for (int c = 0; c < a.nchain; c++) ok &= strlen(a.chain[c]) < DNS_NAME_SIZE;
        } else {
            ok &= r == DNS_E_NOTOURS || r == DNS_E_BADREPLY || r == DNS_E_LOOP;
        }
        free(p);
    }
    CHECK(ok, "300000 mangled DNS answers: no crash, nothing out of bounds");
    CHECK(parsed > 1000, "and plenty of them still parsed");
}

// ---- SNTP ----

static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

#define NONCE 0x0123456789ABCDEFull
#define T(s, f) ((uint64_t)(s) << 32 | (uint32_t)(f))  // fixed point from seconds and a fraction
#define NOW_NTP (1794445077u + NTP_UNIX_EPOCH)        // 2026-11-12 in seconds since 1900

// A server's reply to the request carrying NONCE: version 4, stratum 2,
// received at NOW_NTP + 1/4 s and sent at NOW_NTP + 1/2 s.
static void good_reply(uint8_t* p) {
    memset(p, 0, NTP_SIZE);
    p[0] = 0 << 6 | 4 << 3 | 4;
    p[1] = 2;
    p[2] = 6;
    p[3] = (uint8_t)-20;
    wr32(p + 12, 0xC0000201);
    wr64(p + 16, T(NOW_NTP - 30, 0));
    wr64(p + 24, NONCE);
    wr64(p + 32, T(NOW_NTP, 0x40000000));
    wr64(p + 40, T(NOW_NTP, 0x80000000));
}

static void test_ntp_request(void) {
    uint8_t p[NTP_SIZE];
    memset(p, 0xEE, sizeof p);
    ntp_build_request(p, NONCE);
    CHECK(p[0] == 0x23, "version 4, client mode, no leap warning");
    int zero = 1;
    for (int i = 1; i < 40; i++) zero &= p[i] == 0;
    CHECK(zero, "everything else zero: nothing about our clock goes out");
    CHECK(p[40] == 0x01 && p[43] == 0x67 && p[47] == 0xEF, "the nonce as the transmit timestamp, big-endian");
}

static void test_ntp_parse(void) {
    uint8_t p[NTP_SIZE + 20], q[NTP_SIZE + 20];
    ntp_reply_t r;
    good_reply(p);
    CHECK(ntp_parse(p, NTP_SIZE, NONCE, &r) == 0, "a good reply");
    CHECK(r.leap == 0 && r.version == 4 && r.stratum == 2 && r.refid == 0xC0000201, "its header");
    CHECK(r.recv == T(NOW_NTP, 0x40000000) && r.xmit == T(NOW_NTP, 0x80000000), "its timestamps");

    memcpy(q, p, NTP_SIZE);
    memset(q + NTP_SIZE, 0xAB, 20);
    CHECK(ntp_parse(q, NTP_SIZE + 20, NONCE, &r) == 0, "extensions or a MAC after it are ignored");
    CHECK(ntp_parse(p, NTP_SIZE - 1, NONCE, &r) == NTP_E_NOTOURS, "47 bytes is not a reply");
    CHECK(ntp_parse(p, 0, NONCE, &r) == NTP_E_NOTOURS, "nor is nothing");
    CHECK(ntp_parse(p, NTP_SIZE, NONCE ^ 1, &r) == NTP_E_NOTOURS, "a reply to another request (one bit of the nonce)");
    CHECK(ntp_parse(p, NTP_SIZE, NONCE ^ (1ull << 63), &r) == NTP_E_NOTOURS, "(the top bit)");
    for (int mode = 0; mode < 8; mode++) {
        if (mode == 4) continue;
        memcpy(q, p, NTP_SIZE);
        q[0] = (uint8_t)((q[0] & ~7) | mode);
        CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_NOTOURS, "only mode 4 is a server's reply");
    }
    uint8_t req[NTP_SIZE];
    ntp_build_request(req, NONCE);
    memcpy(req + 24, req + 40, 8);  // our own request bounced back, origin and all
    CHECK(ntp_parse(req, NTP_SIZE, NONCE, &r) == NTP_E_NOTOURS, "our request reflected back");

    memcpy(q, p, NTP_SIZE);
    q[1] = 0;
    memcpy(q + 12, "RATE", 4);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_KISS && !strcmp(r.kiss, "RATE"), "stratum 0 is a kiss-o'-death");
    memcpy(q + 12, "\x1b[2J", 4);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_KISS && !memcmp(r.kiss, "\x1b[2J", 5), "its code as sent");
    wr64(q + 24, NONCE + 1);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_NOTOURS, "and not believed unless it is ours");

    memcpy(q, p, NTP_SIZE);
    q[0] |= 3 << 6;
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_UNSYNC, "leap indicator 3: the server is not synchronised");
    for (int li = 1; li <= 2; li++) {
        memcpy(q, p, NTP_SIZE);
        q[0] = (uint8_t)(q[0] | li << 6);
        CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == 0 && r.leap == li, "a leap second warning is fine");
    }
    int strata[] = { 1, 15 };
    for (int i = 0; i < 2; i++) {
        memcpy(q, p, NTP_SIZE);
        q[1] = (uint8_t)strata[i];
        CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == 0 && r.stratum == strata[i], "strata 1 to 15");
    }
    int bad_strata[] = { 16, 17, 255 };
    for (int i = 0; i < 3; i++) {
        memcpy(q, p, NTP_SIZE);
        q[1] = (uint8_t)bad_strata[i];
        CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_UNSYNC, "stratum 16 and up: unsynchronised");
    }
    for (int v = 0; v < 8; v++) {
        memcpy(q, p, NTP_SIZE);
        q[0] = (uint8_t)((q[0] & ~0x38) | v << 3);
        int e = ntp_parse(q, NTP_SIZE, NONCE, &r);
        CHECK(v >= 1 && v <= 4 ? e == 0 && r.version == v : e == NTP_E_BADREPLY, "versions 1 to 4 only");
    }
    memcpy(q, p, NTP_SIZE);
    wr64(q + 32, 0);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_BADREPLY, "no receive timestamp");
    memcpy(q, p, NTP_SIZE);
    wr64(q + 40, 0);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == NTP_E_BADREPLY, "no transmit timestamp");
    memcpy(q, p, NTP_SIZE);
    wr64(q + 32, 1);
    CHECK(ntp_parse(q, NTP_SIZE, NONCE, &r) == 0, "a timestamp of one fraction is not zero");
}

static void test_ntp_time(void) {
    uint32_t now = 1794445077u;  // 2026-11-12 03:37:57
    CHECK(ntp_to_unix(T(NOW_NTP, 5), now) == T(now, 5), "now is now, fraction and all");
    CHECK(ntp_to_unix(T(NTP_UNIX_EPOCH, 0), now) == T(0, 0) - 0 && ntp_to_unix(T(NTP_UNIX_EPOCH, 7), now) == 7,
          "1970 is 0");
    // Era 1 starts at 2036-02-07 06:28:16, Unix 2085978496.
    CHECK(ntp_to_unix(T(0, 0), now) == T(2085978496u, 0), "seconds 0 seen from 2026 is the 2036 wrap");
    CHECK(ntp_to_unix(T(0xFFFFFFFFu, 0), now) == T(2085978495u, 0), "and the second before it is in era 0");
    CHECK(ntp_to_unix(T(1000, 0), 2085978000u) == T(2085979496u, 0), "seen from just before the wrap");
    CHECK(ntp_to_unix(T(0xFFFFFF00u, 0), 2085979000u) == T(2085978240u, 0), "and era 0 seen from just after");
    CHECK(ntp_to_unix(T(NOW_NTP, 0), 0) == T(now, 0), "2026 seen from a clock reset to 1970");
    CHECK(ntp_to_unix(T(123010304u, 0), 0) == 0, "2040 from 1970 is more than 68 years: refused, not 1904");
    CHECK(ntp_to_unix(T(123010304u, 0), now) == T(123010304u + 2085978496u, 0), "but from 2026 it is 2040");
    CHECK(ntp_to_unix(T(NTP_UNIX_EPOCH - 1, 0), 100) == 0, "1969 is refused");
    // Unix 2^32 is NTP seconds 2^32 + NTP_UNIX_EPOCH, which wraps to NTP_UNIX_EPOCH.
    CHECK(ntp_to_unix(T(NTP_UNIX_EPOCH, 0), 0xFFFFFF00u) == 0, "and 2106 past 32 bits");
    CHECK(ntp_to_unix(T(NTP_UNIX_EPOCH + 5, 0), 0xFFFFFF00u) == 0, "(not just where it wraps to 0)");
    CHECK(ntp_to_unix(T(NTP_UNIX_EPOCH - 1, 9), 0xFFFFFF00u) == T(0xFFFFFFFFu, 9), "but its last second is not");

    int64_t off, delay;
    ntp_offset(T(100, 0), T(110, 0x80000000), T(110, 0x80000000), T(101, 0), &off, &delay);
    CHECK(off == (int64_t)T(10, 0) && delay == (int64_t)T(1, 0), "ten seconds slow, a second's round trip");
    ntp_offset(T(1000, 0), T(900, 0x40000000), T(900, 0xC0000000), T(1001, 0), &off, &delay);
    // ((900.25 - 1000) + (900.75 - 1001)) / 2 = -100; 1 s round trip less the server's 0.5 s
    CHECK(off == -(int64_t)T(100, 0) && delay == (int64_t)T(0, 0x80000000), "fast, with the server taking half a second");
    ntp_offset(T(5, 0), T(5, 0), T(9, 0), T(6, 0), &off, &delay);
    CHECK(delay == 0, "a delay below zero (the server's clock ran fast) counts as none");
    uint64_t t1 = T(0, 0), t2 = T(now, 0), t4 = T(0, 0x10000000);
    ntp_offset(t1, t2, t2, t4, &off, &delay);
    CHECK(off == (int64_t)T(now, 0) - 0x08000000 && delay == 0x10000000, "56 years off, without overflow");
    ntp_offset(t2, t1, t1, t2, &off, &delay);
    CHECK(off == -(int64_t)T(now, 0) && delay == 0, "and the other way");
}

static void fuzz_ntp(void) {
    uint8_t base[NTP_SIZE + 20];
    good_reply(base);
    memset(base + NTP_SIZE, 0, 20);
    int ok = 1, parsed = 0;
    for (int i = 0; i < 300000; i++) {
        uint8_t m[NTP_SIZE + 20];
        memcpy(m, base, sizeof m);
        for (int k = 1 + (int)(rnd() % 4); k > 0; k--) {
            int at = (int)(rnd() % (i % 3 ? 24u : sizeof m));  // mostly the header, which is where the checks are
            if (rnd() % 2) m[at] = (uint8_t)rnd();
            else m[at] ^= (uint8_t)(1 << (rnd() % 8));
        }
        int len = rnd() % 4 ? NTP_SIZE : (int)(rnd() % (sizeof m + 1));
        uint8_t* p = (uint8_t*)malloc((size_t)len + 1);
        memcpy(p, m, (size_t)len);
        ntp_reply_t r;
        int e = ntp_parse(p, len, NONCE, &r);
        if (e == 0) {
            parsed++;
            ok &= len >= NTP_SIZE && (p[0] & 7) == 4 && !memcmp(p + 24, base + 24, 8) && r.leap < 3 &&
                  r.version >= 1 && r.version <= 4 && r.stratum >= 1 && r.stratum <= 15 && r.recv && r.xmit;
        } else if (e == NTP_E_KISS) {
            ok &= p[1] == 0 && !memcmp(p + 24, base + 24, 8) && r.kiss[4] == 0;
        } else {
            ok &= e == NTP_E_NOTOURS || e == NTP_E_BADREPLY || e == NTP_E_UNSYNC;
        }
        free(p);
    }
    CHECK(ok, "300000 mangled SNTP replies: no crash, nothing believed that should not be");
    CHECK(parsed > 10000, "and plenty of them still parsed");
    int any = 0;
    for (int i = 0; i < 100000; i++) {
        uint8_t m[NTP_SIZE];
        for (int k = 0; k < NTP_SIZE; k++) m[k] = (uint8_t)rnd();
        ntp_reply_t r;
        any |= ntp_parse(m, NTP_SIZE, NONCE, &r) != NTP_E_NOTOURS;
    }
    CHECK(!any, "100000 random messages: none taken for ours");
}

// Every day from 1970 to 2105, against a calendar counted a day at a time.
static void test_civil(void) {
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned y = 1970, mo = 1, d = 1;
    int ok = 1;
    uint32_t day = 0;
    for (; y <= CIVIL_MAX_YEAR; day++) {
        rtc_time_t t = { y, mo, d, rnd() % 24, rnd() % 60, rnd() % 60 }, u;
        uint32_t s = day * 86400u + t.hour * 3600 + t.min * 60 + t.sec;
        ok &= civil_valid(&t) && civil_to_unix(&t) == s;
        unix_to_civil(s, &u);
        ok &= !memcmp(&t, &u, sizeof t);
        if (!ok) { printf("  at %u-%u-%u\n", y, mo, d); break; }
        int leap = y % 4 == 0 && (y % 100 || y % 400 == 0);
        if (++d > (unsigned)(mdays[mo - 1] + (mo == 2 && leap))) {
            d = 1;
            if (++mo > 12) { mo = 1; y++; }
        }
    }
    CHECK(ok && day == 49673, "every day from 1970 to 2105, both ways");
    rtc_time_t last;
    unix_to_civil(0xFFFFFFFFu, &last);
    CHECK(last.year == 2106 && last.month == 2 && last.day == 7 && last.hour == 6 && last.min == 28 && last.sec == 15,
          "the last second 32 bits hold");
    rtc_time_t good[] = { { 2000, 2, 29, 0, 0, 0 }, { 2024, 2, 29, 23, 59, 59 }, { 1970, 1, 1, 0, 0, 0 },
                          { 2105, 12, 31, 23, 59, 59 }, { 2026, 4, 30, 12, 0, 0 } };
    rtc_time_t bad[] = { { 2100, 2, 29, 0, 0, 0 }, { 2026, 2, 29, 0, 0, 0 }, { 2026, 4, 31, 0, 0, 0 }, { 2026, 0, 1, 0, 0, 0 },
                         { 2026, 13, 1, 0, 0, 0 }, { 2026, 1, 0, 0, 0, 0 }, { 2026, 1, 32, 0, 0, 0 }, { 2026, 1, 1, 24, 0, 0 },
                         { 2026, 1, 1, 0, 60, 0 }, { 2026, 1, 1, 0, 0, 60 }, { 1969, 12, 31, 23, 59, 59 },
                         { 2106, 1, 1, 0, 0, 0 }, { 0xFFFFFFFFu, 1, 1, 0, 0, 0 }, { 2026, 0xFFFFFFFFu, 1, 0, 0, 0 } };
    for (unsigned i = 0; i < sizeof good / sizeof *good; i++) CHECK(civil_valid(&good[i]), "a real moment");
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!civil_valid(&bad[i]), "not a real moment (or out of range)");
}

int main(void) {
    test_dhcp_build();
    test_dhcp_parse();
    fuzz_dhcp();
    test_dns_names();
    test_dns_parse();
    fuzz_dns();
    test_ntp_request();
    test_ntp_parse();
    test_ntp_time();
    fuzz_ntp();
    test_civil();
    printf("test_netmsg: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

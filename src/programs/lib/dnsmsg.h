// DNS messages (RFC 1035): a query built and an answer read, with no
// system calls, so the host tests can feed the parser whatever they
// like. The resolver that sends them is dns.h.
#ifndef RINGTAIL_DNSMSG_H
#define RINGTAIL_DNSMSG_H

#include <stdint.h>

#define DNS_PORT 53
#define DNS_TYPE_A     1
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_PTR   12

#define DNS_RCODE_FORMERR  1
#define DNS_RCODE_SERVFAIL 2
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_NOTIMP   4
#define DNS_RCODE_REFUSED  5

#define DNS_NAME_SIZE 256  // a name as text, with its NUL
#define DNS_QUERY_MAX 272  // the header, the longest name and the type and class
#define DNS_MAX_CHAIN 8    // aliases followed, at most
#define DNS_MAX_ADDRS 8    // addresses kept, at most

// Errors, below the errno values so the two never meet.
#define DNS_E_NONAME    -1001  // NXDOMAIN
#define DNS_E_NODATA    -1002  // the name is there but has no address
#define DNS_E_SERVFAIL  -1003
#define DNS_E_REFUSED   -1004
#define DNS_E_FORMERR   -1005  // FORMERR, NOTIMP or a code nobody has heard of
#define DNS_E_NOSERVER  -1006
#define DNS_E_BADNAME   -1007
#define DNS_E_BADREPLY  -1008  // an answer to our question that makes no sense
#define DNS_E_TIMEOUT   -1009
#define DNS_E_TRUNCATED -1010  // too big for UDP (the resolver does not retry over TCP)
#define DNS_E_LOOP      -1011  // aliases that go on too long, or round in a circle
#define DNS_E_NOTOURS   -1012  // not the answer to this question: ignore it

// What an answer said. chain[i] is what the name before it (the one
// asked, for chain[0]) is an alias for; the addresses, or the PTR
// name, belong to the last name in the chain.
typedef struct {
    int rcode;      // 0, or DNS_RCODE_*
    int truncated;  // the server set TC
    int nchain;
    char chain[DNS_MAX_CHAIN][DNS_NAME_SIZE];
    int naddrs;
    uint32_t addrs[DNS_MAX_ADDRS];  // host order, no repeats
    char ptr[DNS_NAME_SIZE];        // a PTR question's answer, or ""
} dns_answer_t;

// A name we may ask about: labels of letters, digits, '-' and '_', 1 to
// 63 long, 253 in all, and an optional trailing dot.
int dns_name_ok(const char* name);
// Two names the same, ignoring case and a trailing dot.
int dns_name_eq(const char* a, const char* b);
// A query for name (recursion wanted), in p, which has size bytes.
// Returns the length, or DNS_E_BADNAME.
int dns_build_query(uint8_t* p, int size, uint16_t id, const char* name, int type);
// Reads the len bytes at p as the answer to query id, which asked for
// name and type. 0 when it is, with a filled in (look at a->rcode);
// DNS_E_NOTOURS when it is some other message; DNS_E_BADREPLY or
// DNS_E_LOOP when it is ours but unusable.
int dns_parse(const uint8_t* p, int len, uint16_t id, const char* name, int type, dns_answer_t* a);

#endif

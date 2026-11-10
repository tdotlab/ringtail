// The resolver: names to addresses, and back, by asking the DNS servers
// the network was given (see `net`) over UDP. No cache, and no retry over TCP.
#ifndef RINGTAIL_DNS_H
#define RINGTAIL_DNS_H

#include "dnsmsg.h"

typedef struct {
    char name[DNS_NAME_SIZE];  // what was found: the name asked, perhaps with the search domain added
    dns_answer_t a;            // the aliases and addresses (or the PTR name) found for it
} dns_result_t;

// The addresses for name. A dotted address is its own answer, and
// "localhost" is 127.0.0.1, with no question asked. A name with no dot
// is tried with the search domain first. server 0 means the configured
// servers; port 0 means 53. Returns how many addresses, or a negative
// DNS_E_* or errno.
int dns_lookup(const char* name, uint32_t server, int port, dns_result_t* r);
// The name for ip (a PTR question for d.c.b.a.in-addr.arpa, whose text
// ends up in r->name). 0, or a negative DNS_E_* or errno.
int dns_reverse(uint32_t ip, uint32_t server, int port, dns_result_t* r);
// Text for a DNS_E_* or errno value.
const char* dns_strerror(int err);

#endif

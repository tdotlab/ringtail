// Connecting to a host by name: the DNS lookup, then each address it
// gave in turn until one answers.
#ifndef RINGTAIL_DIAL_H
#define RINGTAIL_DIAL_H

#include <stdint.h>

// A connected TCP socket's descriptor, or a negative DNS_E_* or errno
// (dns_strerror has the text). timeout_ms limits each attempt, as for
// SYS_CONNECT. *ip, if given, gets the address that answered, or the
// last one tried.
int dial(const char* host, int port, int timeout_ms, uint32_t* ip);

#endif

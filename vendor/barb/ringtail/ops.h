// What the agent's actions turn into on RingTail: programs, HTTP through fetch, files, timers.
//
// The same shape as src/posix/ops.h. Every operation ends in exactly one event handed to the
// deliver callback (or none, if it was cancelled), and the callback only queues it: it must not
// call the agent, because the agent's actions are still being carried out when these run.
#ifndef BARB_RINGTAIL_OPS_H
#define BARB_RINGTAIL_OPS_H

#include "ulib.h"
#include "../core/agent.h"

typedef void (*deliver_fn)(const ev_t* ev);

// base_url without a trailing slash; ca_file 0 for fetch's own roots.
void ops_init(deliver_fn deliver, const char* base_url, const char* api_key, const char* ca_file);

void ops_http(int64_t id, str_t path, str_t body);
void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log);
void ops_read(int64_t id, str_t path);
void ops_write(int64_t id, str_t path, str_t data);
void ops_timer(int64_t id, int64_t ms);
void ops_cancel(int64_t id);

// For the main loop's sys_pollfd: the descriptors operations want watched (at most
// OPS_MAX_FDS), how long it may sleep, and the call that does what is due.
#define OPS_MAX_FDS 12
int ops_fill(pollfd_t* fds);            // how many it filled in
int ops_timeout_ms(void);               // -1 if nothing is waiting on anything
void ops_service(const pollfd_t* fds, int n);

int64_t ops_now_ms(void);
void ops_shutdown(void);                // stop everything still running, remove the request file

#endif

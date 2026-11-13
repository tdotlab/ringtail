// Tab stops for expand and unexpand: reading the list that -t takes, and
// finding the next stop from a column. Columns count bytes; a tab goes to
// the next stop, and a backspace moves back one (the callers do that).
#ifndef RINGTAIL_TABSTOPS_H
#define RINGTAIL_TABSTOPS_H

#include <stdint.h>

typedef struct {
    uint64_t* stops;   // the stops listed, each beyond the one before
    uint32_t n, cap;
    uint64_t size;     // not 0: a stop at every multiple of size (and no list)
    uint64_t extend;   // /N on the last item: past the list, every multiple of N
    uint64_t incr;     // +N on the last item: past the list, every N from its last stop
} tabs_t;

void tabs_init(tabs_t* t);

// Adds what a -t argument says: numbers separated by commas or blanks,
// each beyond the last. The last number may start with / or +: "4,/8"
// has stops at 4 and then at each multiple of 8, "4,+8" at 4 and then
// every 8 beyond it. A second -t goes on from the first, as in GNU
// expand. Returns 0, or 1 once it has told the user, on standard error
// and as "PROG: ...", what is wrong (only the first complaint is made).
int tabs_add(tabs_t* t, const char* list, const char* prog);

// Call once all the -t are in (or none were). Settles what a list of one
// number, or of none, means: a stop at each multiple of it, or of 8. Past
// the last stop of any other list there are no more, and a tab is a single
// space. Returns 0, or 1 after complaining that / and + were both used.
int tabs_finish(tabs_t* t, const char* prog);

// The first stop beyond column col. *last is set, and 0 returned, when
// there is none.
uint64_t tabs_next(const tabs_t* t, uint64_t col, int* last);

#endif

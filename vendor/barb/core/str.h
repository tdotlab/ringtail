// Strings for the core: a slice type and a growing buffer.
//
// A str_t is a pointer and a length, not NUL-terminated, and does not own
// its bytes. Strings the core makes (str_dup, json_unescape, a finished
// sb_t) do have a NUL after the last byte, which is not counted, so the
// platform can pass them to C APIs; a string with an embedded NUL has to
// be checked by the caller (the core refuses NULs in paths and commands).
#ifndef BARB_STR_H
#define BARB_STR_H

#include <stddef.h>
#include <stdint.h>
#include "arena.h"

typedef struct {
    const char* p;
    size_t n;
} str_t;

#define STR_NONE ((size_t)-1)

str_t str_of(const char* p, size_t n);
str_t str_z(const char* z);                  // a NUL-terminated string; 0 gives an empty one
int str_eq(str_t a, str_t b);
int str_eqz(str_t a, const char* z);
int str_starts(str_t s, str_t prefix);
size_t str_find(str_t hay, str_t needle);    // index of the first match, or STR_NONE
str_t str_dup(arena_t* a, str_t s);          // a copy in the arena, NUL after it; {0,0} on failure
str_t str_trim(str_t s);                     // without leading and trailing space, tab, CR, LF

typedef struct {
    arena_t* a;
    char* p;
    size_t n, cap;
    int failed;
} sb_t;

void sb_init(sb_t* b, arena_t* a);
void sb_put(sb_t* b, const void* p, size_t n);
void sb_puts(sb_t* b, str_t s);
void sb_putz(sb_t* b, const char* z);
void sb_putc(sb_t* b, char c);
void sb_putu(sb_t* b, uint64_t v);
void sb_puti(sb_t* b, int64_t v);
void sb_pad_u(sb_t* b, uint64_t v, int width);  // v right-aligned in `width` columns
// The text so far, NUL-terminated. Valid until the next put. A failed buffer gives {0,0}.
str_t sb_str(sb_t* b);

#endif

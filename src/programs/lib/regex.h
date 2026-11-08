// Regular expressions, POSIX style: the basic and extended syntaxes of
// grep, matched by simulating the automaton, so a pattern can never
// take exponential time. No system calls, so the host tests can run it.
//
// Supported: . [set] [^set] with ranges and [:alpha:]-style classes,
// ^ $ * and, in the basic syntax, \( \) \| \+ \? \{m,n\}, in the
// extended one ( ) | + ? {m,n}; and the GNU word items \w \W \s \S \b
// \B \< \>. Not supported (a message says so): back-references and the
// [. .] and [= =] bracket forms.
//
// Matches are leftmost-longest, as POSIX asks.
#ifndef RINGTAIL_REGEX_H
#define RINGTAIL_REGEX_H

#include <stdint.h>

#define RE_ICASE 1   // letters match in either case
#define RE_ERE   2   // the extended syntax
#define RE_FIXED 4   // the patterns are plain strings, no syntax at all
#define RE_WORD  8   // a match must be a whole word (grep -w)
#define RE_LINE  16  // a match must be the whole line (grep -x)

typedef struct {
    void* prog;      // the compiled pattern and the space to run it in
    void* sets;
    uint16_t* cur;
    uint16_t* nxt;
    uint16_t* stack;
    uint32_t* mark;
    uint32_t gen;
    uint32_t n, cap, nsets, setcap;
    unsigned flags;
} re_t;

// Compiles pat[0..len). Newlines separate several patterns, any of
// which may match. 0 for success, else a short message. Free with
// re_free, also after a failure.
const char* re_compile(re_t* re, const char* pat, uint32_t len, unsigned flags);
void re_free(re_t* re);

// Whether line[0..len) has a match anywhere in it.
int re_test(re_t* re, const char* line, uint32_t len);

// The leftmost, then longest, match that starts at or after `from`:
// 1 with line[*ms .. *me) set, or 0.
int re_find(re_t* re, const char* line, uint32_t len, uint32_t from, uint32_t* ms, uint32_t* me);

#endif

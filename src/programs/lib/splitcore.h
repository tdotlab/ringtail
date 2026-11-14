// The parts of split that are only text and arithmetic: the numbers of its
// options, and the names of the files it writes (xaa, xab, ... or x00, x01,
// ... or hex), as GNU split makes them.
#ifndef RINGTAIL_SPLITCORE_H
#define RINGTAIL_SPLITCORE_H

#include <stdint.h>

// Reads the N of -l N: blanks, an optional +, digits, nothing else. Returns 0
// and the number (a number too big for 64 bits is the biggest one), or -1 for
// anything else, and for 0.
int sp_parse_lines(const char* s, uint64_t* out);

// Reads the SIZE of -b SIZE: the same, then optionally b (512), or k K m M G T
// P E Z Y R Q (powers of 1024), which may be followed by iB (the same) or B
// or D (powers of 1000). A suffix alone means 1 of it ("K" is 1024). 0 and a
// product too big for 64 bits are the same as for lines.
int sp_parse_bytes(const char* s, uint64_t* out);

// Reads the LEN of -a LEN. Returns 0 and the number (at most 4294967295), or
// -1 for text that is no number, -2 for a negative number, -3 for one that
// does not fit a long (GNU says "Numerical result out of range" for -2 and
// "Value too large for defined data type" for -3).
int sp_parse_suffix_len(const char* s, uint32_t* out);

#define SP_ALPHA 0  // aa ab ... zz
#define SP_NUM 1    // 00 01 ... 99
#define SP_HEX 2    // 00 01 ... ff

#define SP_MAXLEN 256  // the most letters a suffix can have here

#define SP_OK 0
#define SP_BAD_START 1        // FROM has a character the alphabet lacks, or none
#define SP_START_TOO_LARGE 2  // FROM does not fit in the suffix length
#define SP_TOO_LONG 3         // a suffix longer than SP_MAXLEN

typedef struct {
    int base;                    // 26, 10 or 16
    int len;                     // letters in the suffix now
    int markers;                 // letters at the front that say "wider": z, 9 or f
    int widen;                   // whether the suffix grows when it runs out of room
    unsigned char dig[SP_MAXLEN];
} sp_suffix_t;

// The first suffix. len 0 is the automatic length: 2 at first, then wider
// (aa..yz, zaaa..zyzz, zzaaaa..) as the files go on. A length of 1 or more
// is fixed, and so is the length that FROM, if there is one (not for
// SP_ALPHA), is a start for: the suffix is its digits, with leading zeros.
// FROM is read as decimal or as lowercase hex, whatever the kind says.
int sp_suffix_init(sp_suffix_t* s, int kind, uint32_t len, const char* from);

// The suffix as text (len letters and a NUL; out needs SP_MAXLEN + 1 bytes).
void sp_suffix_text(const sp_suffix_t* s, char* out);

// Goes on to the next suffix. 0, or -1 when there is none ("output file
// suffixes exhausted").
int sp_suffix_next(sp_suffix_t* s);

#endif

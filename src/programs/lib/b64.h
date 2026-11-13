// Base64 (RFC 4648), a byte at a time, for the base64 program. Decoding
// does what GNU base64 does, down to the bytes it writes before it finds
// something wrong: the input is read in groups of four characters (a
// newline never counts toward one), each group is decoded, and the first
// bad group stops everything after the bytes in it that were good.
#ifndef RINGTAIL_B64_H
#define RINGTAIL_B64_H

#include <stdint.h>

typedef struct {
    uint8_t in[3];
    int n;
    uint32_t wrap, col;  // wrap 0: one long line
} b64enc_t;

// wrap: the line length (0 for no line breaks at all).
void b64enc_init(b64enc_t* e, uint32_t wrap);
// Takes a byte; writes 0 to 8 characters to out (4 and the line breaks
// among them). Returns how many.
int b64enc_put(b64enc_t* e, uint8_t c, char* out);
// Call at the end of the input; out needs room for 9. Returns how many.
int b64enc_end(b64enc_t* e, char* out);

typedef struct {
    char q[4];
    int n;
    int ignore;  // skip everything that is not part of the alphabet (-i)
    int bad;     // set once the input has been found invalid
} b64dec_t;

void b64dec_init(b64dec_t* d, int ignore_garbage);
// Takes a character; writes 0 to 3 bytes to out and returns how many.
// After a call that leaves d->bad set, the bytes returned are the last
// ones to write: the input is not valid and nothing more should be read.
int b64dec_put(b64dec_t* d, uint8_t c, uint8_t* out);
// Call at the end of the input; the same rules. A last group of fewer
// than four characters is invalid (and sets d->bad), but what it holds
// is still written first.
int b64dec_end(b64dec_t* d, uint8_t* out);

// The number after -w: blanks, then digits (a minus sign only on a 0). A
// number past 32 bits becomes 0xFFFFFFFF, one past 63 bits becomes 0, which
// is what GNU does with them. 0, or -1 if s is not a number.
int b64_parse_wrap(const char* s, uint32_t* wrap);

#endif

// The parts of join that are only text and arithmetic: the numbers and
// field specifiers of its options, how a line is cut into fields, and how
// two keys compare, as GNU join does them (in the C locale).
#ifndef RINGTAIL_JOINCORE_H
#define RINGTAIL_JOINCORE_H

#include <stdint.h>

// Reads a field number (the FIELD of -1, -2, -j and of 1.FIELD): optional
// blanks, an optional sign, digits, nothing else, and it must be above 0.
// Returns 0 and the number less one (fields are counted from 0 inside); a
// number too big for 63 bits counts as the biggest one. Returns -1 for
// anything else, and leaves *out alone.
int jn_parse_field(const char* s, uint64_t* out);

// Reads the N of -a N and -v N: the same kind of number, and only 1 or 2
// are good. Returns 1 or 2, or 0 for anything else.
int jn_parse_filenum(const char* s);

#define JN_OK 0
#define JN_BAD_SPEC 1   // "invalid field specifier"
#define JN_BAD_FILE 2   // "invalid file number in field spec"
#define JN_BAD_FIELD 3  // "invalid field number" (of the part after "1.")

// Reads one item of an -o list: "0" (the join field), "1.N" or "2.N".
// Sets *file (0, 1 or 2) and *field (counted from 0; 0 for "0").
int jn_decode_spec(const char* s, int* file, uint64_t* field);

typedef struct {
    uint32_t beg, len;
} jn_field_t;

// tab: -1 splits at runs of blanks (space, tab, newline) and ignores the
// blanks at the start; a character splits at each of it; '\n' (what an empty
// -t means) leaves the whole line as one field.
#define JN_BLANKS (-1)

// Cuts the n bytes at s into fields. Writes the first `cap` of them to out
// and returns how many there are (an empty line has none; a line with blanks
// at its end has an empty field after them, as in GNU join).
uint32_t jn_split(const char* s, uint32_t n, int tab, jn_field_t* out, uint32_t cap);

// Compares two keys: below 0, 0 or above 0. An empty key sorts before any
// other (and the same as an empty one); otherwise bytes, then length.
// icase compares a-z as A-Z.
int jn_keycmp(const char* a, uint32_t alen, const char* b, uint32_t blen, int icase);

// The number as decimal text (out needs 21 bytes).
void jn_u64_text(uint64_t v, char* out);

#endif

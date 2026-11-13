// The lines of a checksum list, as sha256 -c reads them: what GNU
// sha256sum -c makes of each line, down to the odd ones (a tab for the
// blank, a mode character, a name in backslash form, a BSD "SHA256 (NAME)
// = HASH" line, a line cut short by a NUL byte).
#ifndef RINGTAIL_SHASUM_H
#define RINGTAIL_SHASUM_H

#include <stdint.h>

#define SHASUM_SKIP 0     // an empty line, or one that starts with "#"
#define SHASUM_LINE 1     // a checksum and a name
#define SHASUM_IMPROPER -1

// How the lines of a list say "text" or "binary", as GNU settles it for the
// whole run from the first line that shows it: after the blank that follows
// the hash, either every line has a mode character (" " or "*") and then
// the name, or none has and the name starts right there. A style is set by
// the first line with something after that blank; start with SHASUM_STYLE_NONE
// and keep the same variable for every line of every list.
#define SHASUM_STYLE_NONE 0
#define SHASUM_STYLE_MODE 1
#define SHASUM_STYLE_PLAIN 2

// Reads one line of a list (without its newline). On SHASUM_LINE, hash
// holds the 32 bytes and name the file name, NUL-terminated (name needs
// n + 1 bytes of room), with its length in *namelen: the escapes of a name
// in backslash form are already undone, so a name may have a newline in it.
// *style is the list-wide style above, which this may set.
int shasum_parse(const char* line, uint32_t n, int* style, uint8_t hash[32], char* name, uint32_t* namelen);

// Whether a name must be written in backslash form (a "\" in front of the
// line, then "\\" for a backslash, "\n" and "\r" for those two): when it
// holds a backslash, a newline or a carriage return.
int shasum_needs_escape(const char* name, uint32_t n);

#endif

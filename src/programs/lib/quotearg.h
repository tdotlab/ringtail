// Quoting a name for a message the way GNU tools do, so that the messages
// read the same:
//   QA_C       'x' with \\ \' \a \b \f \n \r \t \v and \ooo for the rest
//   QA_ALWAYS  shell style, always quoted: 'x', "x'y", 'a'$'\001''b'
//   QA_FILE    the same, but a plain name that needs no quoting stays bare
#ifndef RINGTAIL_QUOTEARG_H
#define RINGTAIL_QUOTEARG_H

#include <stdint.h>

enum { QA_C, QA_ALWAYS, QA_FILE };

// Room that qa_quote needs for a string of `len` bytes.
#define QA_SPACE(len) (8 * (uint32_t)(len) + 16)

// Writes the quoted text of s (NUL-terminated) to out, which has QA_SPACE
// bytes.
void qa_quote(const char* s, int style, char* out);

#endif

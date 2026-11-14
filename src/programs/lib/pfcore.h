// What printf does with a format and its arguments, as GNU printf does it:
// the directives, the escapes, the numbers read off the arguments, the
// warnings and the errors. The program (util/printf.c) only supplies the
// two places the text goes to.
#ifndef RINGTAIL_PFCORE_H
#define RINGTAIL_PFCORE_H

#include <stdint.h>

typedef struct {
    // Bytes for standard output (they may include NULs).
    void (*out)(void* ctx, const char* s, uint32_t n);
    // A piece of a message for standard error. A message is several pieces
    // in a row, the last one ending in a newline. Whatever is waiting for
    // standard output must go out before the first piece.
    void (*err)(void* ctx, const char* s, uint32_t n);
    void* ctx;
} pf_io_t;

// Prints FORMAT with the nargs arguments in args (the format is used again
// while arguments are left). Returns the exit status: 0, or 1 if a message
// said something was wrong (a bad directive stops the run; a bad number does
// not). A "\c" ends the run at once with status 0, as in GNU printf.
//
// The floating point conversions (%a %e %f %g) and %q are known, but not
// done: they stop the run with a message that says so.
int pf_run(const pf_io_t* io, const char* format, int nargs, char* const* args);

#endif

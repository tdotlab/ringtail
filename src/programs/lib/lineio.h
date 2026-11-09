// Buffered input and output for the text tools (cut, tee, sed, ...).
//
// lio_out buffers what a program writes and sends it in big pieces; it
// exits on a write error (quietly if the reader has gone away), so the
// caller never checks. lio_in reads a stream a line or a byte at a time
// and flushes the output it is given before it blocks for more input, so
// a program at the end of a slow pipeline shows lines as they are
// decided. Anything that runs out of memory exits with a message.
#ifndef RINGTAIL_LINEIO_H
#define RINGTAIL_LINEIO_H

#include <stdint.h>

typedef struct {
    int fd;
    const char* prog;  // for error messages
    uint32_t n;
    char buf[1024];
} lio_out_t;

typedef struct {
    int fd;
    int len, pos;
    int err;           // a negative errno once a read has failed
    char buf[1024];
} lio_in_t;

typedef struct {
    char* s;           // the text, without its newline (not NUL-terminated)
    uint32_t n, cap;
    int had_nl;        // the line ended with a newline (not the end of input)
} lio_line_t;

void lio_out_init(lio_out_t* o, int fd, const char* prog);
void lio_put(lio_out_t* o, const char* p, uint32_t n);
void lio_putc(lio_out_t* o, char c);
void lio_puts(lio_out_t* o, const char* s);
void lio_flush(lio_out_t* o);

void lio_in_init(lio_in_t* in, int fd);
// The next byte, or -1 at the end of the input or on an error (in->err).
// `flush_first`, if not 0, is flushed before the read blocks.
int lio_getc(lio_in_t* in, lio_out_t* flush_first);

// Reads one line into l. 1 for a line, 0 at the end of the input (also
// after an error, in->err). A last line with no newline is returned with
// had_nl 0.
int lio_getline(lio_in_t* in, lio_out_t* flush_first, lio_line_t* l);

// Appends to a line, growing it. Exits when memory runs out.
void lio_line_add(lio_line_t* l, const char* p, uint32_t n, const char* prog);
void lio_line_addc(lio_line_t* l, char c, const char* prog);

#endif

// The input of hexdump and od: the named files (or standard input) read
// as one stream of bytes, with the first `skip` bytes dropped.
//
// A plain file that lies wholly inside the part to skip is stepped over
// without being read. A file that cannot be opened is reported as
// "PROG: NAME: reason" and the stream goes on with the next one; the
// `status` field turns 1 then, for the program to return.
#ifndef RINGTAIL_BSTREAM_H
#define RINGTAIL_BSTREAM_H

#include <stdint.h>

typedef struct {
    const char* prog;      // for error messages
    char** files;
    int nfiles, fi, fd;
    int status;            // 1 once a file could not be opened or read
    int opened;            // files opened so far, the skipped ones too
    uint32_t skip;         // bytes still to drop
    void (*flush)(void);   // called before a message and before a read blocks
    int ilen, ipos;
    unsigned char ib[1024];
} bstream_t;

// With no file names, standard input is read; "-" is standard input too.
void bs_init(bstream_t* b, const char* prog, char** files, int nfiles, uint32_t skip, void (*flush)(void));

// Opens the first file that can be opened and drops the `skip` bytes
// now, rather than at the first read. 1 if the skip was met (and also if
// no file could be opened: that has been reported already), 0 if the
// input ended inside it.
int bs_start(bstream_t* b);

// Copies up to `want` bytes into dst; fewer only at the end of the input.
uint32_t bs_get(bstream_t* b, unsigned char* dst, uint32_t want);

#endif

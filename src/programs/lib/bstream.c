// See bstream.h.
#include "ulib.h"
#include "bstream.h"

void bs_init(bstream_t* b, const char* prog, char** files, int nfiles, uint32_t skip, void (*flush)(void)) {
    b->prog = prog;
    b->files = files;
    b->nfiles = nfiles;
    b->fi = 0;
    b->fd = -1;
    b->status = 0;
    b->opened = 0;
    b->skip = skip;
    b->flush = flush;
    b->ilen = b->ipos = 0;
}

static void bad(bstream_t* b, const char* name, int e) {
    if (b->flush) b->flush();
    dprintf(STDERR_FILENO, "%s: %s: %s\n", b->prog, name, strerror(e));
    b->status = 1;
}

// Opens the next file; a plain file that lies wholly inside the part
// to skip is stepped over without being read.
static int open_next(bstream_t* b) {
    while (b->fi < b->nfiles || (b->fi == 0 && b->nfiles == 0)) {
        const char* name = b->nfiles ? b->files[b->fi] : "-";
        b->fi++;
        int f = !strcmp(name, "-") ? STDIN_FILENO : sys_open(name, O_RDONLY);
        if (f < 0) { bad(b, name, f); continue; }
        b->opened++;
        // Not for standard input: it may be named again, and has been read some already.
        if (b->skip && f != STDIN_FILENO && sys_fdtype(f) == FDTYPE_FILE) {
            int end = sys_seek(f, 0, SEEK_END);
            if (end >= 0 && (uint32_t)end <= b->skip) {
                b->skip -= (uint32_t)end;
                sys_close(f);
                continue;
            }
            if (end >= 0 && sys_seek(f, (int)b->skip, SEEK_SET) == (int)b->skip) b->skip = 0;
            else sys_seek(f, 0, SEEK_SET);
        }
        b->fd = f;
        return 1;
    }
    return 0;
}

static int refill(bstream_t* b) {
    if (b->flush) b->flush();  // lines show up as the bytes arrive
    for (;;) {
        if (b->fd < 0 && !open_next(b)) return 0;
        int n = sys_read(b->fd, b->ib, sizeof b->ib);
        if (n <= 0) {
            if (n < 0) bad(b, b->fi > 0 && b->nfiles ? b->files[b->fi - 1] : "standard input", n);
            if (b->fd != STDIN_FILENO) sys_close(b->fd);
            b->fd = -1;
            continue;
        }
        b->ilen = n;
        b->ipos = 0;
        if (b->skip) {
            uint32_t d = b->skip < (uint32_t)n ? b->skip : (uint32_t)n;
            b->ipos = (int)d;
            b->skip -= d;
        }
        return 1;
    }
}

uint32_t bs_get(bstream_t* b, unsigned char* dst, uint32_t want) {
    uint32_t got = 0;
    while (got < want) {
        if (b->ipos == b->ilen && !refill(b)) break;
        uint32_t take = (uint32_t)(b->ilen - b->ipos);
        if (take > want - got) take = want - got;
        memcpy(dst + got, b->ib + b->ipos, take);
        b->ipos += (int)take;
        got += take;
    }
    return got;
}

int bs_start(bstream_t* b) {
    if (b->fd < 0 && b->fi == 0) open_next(b);
    while (b->skip && refill(b)) {}
    return !b->opened || !b->skip;
}

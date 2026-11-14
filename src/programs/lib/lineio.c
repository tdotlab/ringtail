#include "ulib.h"
#include "lineio.h"

void lio_out_init(lio_out_t* o, int fd, const char* prog) {
    o->fd = fd;
    o->prog = prog;
    o->n = 0;
}

void lio_flush(lio_out_t* o) {
    const char* p = o->buf;
    while (o->n) {
        int w = sys_write(o->fd, p, o->n);
        if (w <= 0) {
            if (w != -EPIPE) dprintf(STDERR_FILENO, "%s: write error: %s\n", o->prog, w ? strerror(w) : "no progress");
            sys_exit(1);
        }
        p += w;
        o->n -= (uint32_t)w;
    }
}

void lio_put(lio_out_t* o, const char* p, uint32_t n) {
    while (n) {
        uint32_t room = (uint32_t)sizeof o->buf - o->n;
        uint32_t take = room < n ? room : n;
        memcpy(o->buf + o->n, p, take);
        o->n += take;
        p += take;
        n -= take;
        if (o->n == sizeof o->buf) lio_flush(o);
    }
}

void lio_putc(lio_out_t* o, char c) { lio_put(o, &c, 1); }
void lio_puts(lio_out_t* o, const char* s) { lio_put(o, s, (uint32_t)strlen(s)); }

void lio_in_init(lio_in_t* in, int fd) {
    in->fd = fd;
    in->len = in->pos = in->err = 0;
}

int lio_getc(lio_in_t* in, lio_out_t* flush_first) {
    if (in->pos == in->len) {
        if (in->err) return -1;
        if (flush_first) lio_flush(flush_first);
        in->len = sys_read(in->fd, in->buf, sizeof in->buf);
        in->pos = 0;
        if (in->len <= 0) {
            if (in->len < 0) in->err = in->len;
            in->len = 0;
            return -1;
        }
    }
    return (unsigned char)in->buf[in->pos++];
}

void lio_line_add(lio_line_t* l, const char* p, uint32_t n, const char* prog) {
    if (l->n + n > l->cap) {
        uint32_t want = l->cap ? l->cap : 256;
        while (want < l->n + n) {
            if (want > 0x10000000u) {
                dprintf(STDERR_FILENO, "%s: out of memory\n", prog);
                sys_exit(1);
            }
            want *= 2;
        }
        char* q = realloc(l->s, want);
        if (!q) {
            dprintf(STDERR_FILENO, "%s: out of memory\n", prog);
            sys_exit(1);
        }
        l->s = q;
        l->cap = want;
    }
    memcpy(l->s + l->n, p, n);
    l->n += n;
}

void lio_line_addc(lio_line_t* l, char c, const char* prog) { lio_line_add(l, &c, 1, prog); }

int lio_getline(lio_in_t* in, lio_out_t* flush_first, lio_line_t* l) {
    return lio_getline_delim(in, flush_first, l, '\n');
}

int lio_getline_delim(lio_in_t* in, lio_out_t* flush_first, lio_line_t* l, char delim) {
    l->n = 0;
    l->had_nl = 0;
    int any = 0;
    for (;;) {
        // Take what is buffered up to a newline in one piece.
        if (in->pos == in->len) {
            if (lio_getc(in, flush_first) < 0) return any;
            in->pos--;
        }
        any = 1;
        const char* start = in->buf + in->pos;
        const char* end = in->buf + in->len;
        const char* p = start;
        while (p < end && *p != delim) p++;
        lio_line_add(l, start, (uint32_t)(p - start), flush_first ? flush_first->prog : "input");
        in->pos += (int)(p - start);
        if (p < end) {
            in->pos++;
            l->had_nl = 1;
            return 1;
        }
    }
}

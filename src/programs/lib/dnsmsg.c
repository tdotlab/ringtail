// DNS messages: see dnsmsg.h.
#include "dnsmsg.h"
#include "lib.h"

#define CLASS_IN 1
#define HLEN 12

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static int name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

int dns_name_ok(const char* name) {
    size_t len = strlen(name);
    if (len && name[len - 1] == '.') len--;
    if (len == 0 || len > 253) return 0;
    size_t label = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (label == 0) return 0;
            label = 0;
        } else if (!name_char(name[i]) || ++label > 63) return 0;
    }
    return label > 0;
}

int dns_name_eq(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 1 && a[la - 1] == '.') la--;
    if (lb > 1 && b[lb - 1] == '.') lb--;
    if (la != lb) return 0;
    for (size_t i = 0; i < la; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

int dns_build_query(uint8_t* p, int size, uint16_t id, const char* name, int type) {
    if (!dns_name_ok(name) || size < DNS_QUERY_MAX) return DNS_E_BADNAME;
    memset(p, 0, HLEN);
    wr16(p, id);
    p[2] = 0x01;  // RD: the server is to do the work
    wr16(p + 4, 1);
    int o = HLEN;
    for (const char* s = name; *s;) {
        const char* e = s;
        while (*e && *e != '.') e++;
        p[o++] = (uint8_t)(e - s);
        memcpy(p + o, s, (size_t)(e - s));
        o += (int)(e - s);
        s = *e ? e + 1 : e;
    }
    p[o++] = 0;
    wr16(p + o, (uint16_t)type);
    wr16(p + o + 2, CLASS_IN);
    return o + 4;
}

// Appends one byte of a label as text: as it is if it could be in a
// name we would ask for, else escaped the way a zone file writes it.
static int put_char(char* out, int o, uint8_t c) {
    if (name_char((char)c) || c == '*') {
        if (o + 1 >= DNS_NAME_SIZE) return -1;
        out[o++] = (char)c;
    } else if (c == '.' || c == '\\') {
        if (o + 2 >= DNS_NAME_SIZE) return -1;
        out[o++] = '\\';
        out[o++] = (char)c;
    } else {
        if (o + 4 >= DNS_NAME_SIZE) return -1;
        out[o++] = '\\';
        out[o++] = (char)('0' + c / 100);
        out[o++] = (char)('0' + c / 10 % 10);
        out[o++] = (char)('0' + c % 10);
    }
    return o;
}

// The name at off, as text. *next is where whatever follows it starts.
// A compression pointer must point before the stretch of labels it
// ends, so every jump goes further back and a loop cannot happen.
static int read_name(const uint8_t* p, int len, int off, char* out, int* next) {
    int o = 0, wire = 1, jumped = 0, limit = off;
    for (;;) {
        if (off >= len) return -1;
        int n = p[off];
        if ((n & 0xC0) == 0xC0) {
            if (off + 1 >= len) return -1;
            int to = (n & 0x3F) << 8 | p[off + 1];
            if (to >= limit) return -1;
            if (!jumped) *next = off + 2;
            jumped = 1;
            off = limit = to;
            continue;
        }
        if (n & 0xC0) return -1;  // label types 01 and 10 never caught on
        off++;
        if (n == 0) break;
        if (off + n > len || (wire += n + 1) > 255) return -1;
        if (o) {
            if (o + 1 >= DNS_NAME_SIZE) return -1;
            out[o++] = '.';
        }
        for (int i = 0; i < n; i++)
            if ((o = put_char(out, o, p[off + i])) < 0) return -1;
        off += n;
    }
    if (!jumped) *next = off;
    if (o == 0) out[o++] = '.';  // the root
    out[o] = 0;
    return 0;
}

typedef struct {
    char owner[DNS_NAME_SIZE];
    int type, class, rd, rdlen;
} rr_t;

// The record at *off, which then moves past it. -1 if it runs off the
// end or its data does not fit its type.
static int read_rr(const uint8_t* p, int len, int* off, rr_t* rr) {
    if (read_name(p, len, *off, rr->owner, off) || *off + 10 > len) return -1;
    rr->type = rd16(p + *off);
    rr->class = rd16(p + *off + 2);
    rr->rdlen = rd16(p + *off + 8);
    rr->rd = *off + 10;
    if (rr->rd + rr->rdlen > len) return -1;
    *off = rr->rd + rr->rdlen;
    if (rr->class != CLASS_IN) return 0;
    if (rr->type == DNS_TYPE_A && rr->rdlen != 4) return -1;
    if (rr->type == DNS_TYPE_CNAME || rr->type == DNS_TYPE_PTR) {
        char name[DNS_NAME_SIZE];
        int end;
        if (read_name(p, rr->rd + rr->rdlen, rr->rd, name, &end) || end != rr->rd + rr->rdlen) return -1;
    }
    return 0;
}

int dns_parse(const uint8_t* p, int len, uint16_t id, const char* name, int type, dns_answer_t* a) {
    memset(a, 0, sizeof *a);
    if (len < HLEN || rd16(p) != id) return DNS_E_NOTOURS;
    if (!(p[2] & 0x80) || (p[2] >> 3 & 15) != 0) return DNS_E_NOTOURS;  // a question, or not a query at all
    a->truncated = (p[2] & 0x02) != 0;
    a->rcode = p[3] & 15;
    int qd = rd16(p + 4), an = rd16(p + 6), off = HLEN;
    if (qd == 0 && a->rcode) return 0;  // some servers leave the question out of an error
    if (qd != 1) return DNS_E_BADREPLY;
    char q[DNS_NAME_SIZE];
    if (read_name(p, len, off, q, &off) || off + 4 > len) return DNS_E_BADREPLY;
    if (!dns_name_eq(q, name) || rd16(p + off) != type || rd16(p + off + 2) != CLASS_IN) return DNS_E_NOTOURS;
    off += 4;
    if (a->rcode) return 0;

    // Every record is checked once; after that they can be read again
    // without fear. A truncated answer keeps the records that came whole.
    int start = off, count = 0;
    rr_t rr;
    for (; count < an; count++) {
        int was = off;
        if (read_rr(p, len, &off, &rr)) {
            if (!a->truncated) return DNS_E_BADREPLY;
            off = was;
            break;
        }
    }
    int end = off;

    // Follow the aliases, then take what the last name has.
    const char* target = name;
    for (;;) {
        int found = 0;
        for (off = start; off < end && !found;) {
            read_rr(p, len, &off, &rr);  // as in the first pass, so it cannot fail now
            if (rr.class != CLASS_IN || rr.type != DNS_TYPE_CNAME || !dns_name_eq(rr.owner, target)) continue;
            if (a->nchain == DNS_MAX_CHAIN) return DNS_E_LOOP;
            int e;
            read_name(p, rr.rd + rr.rdlen, rr.rd, a->chain[a->nchain], &e);
            target = a->chain[a->nchain++];
            found = 1;
        }
        if (!found) break;
    }
    for (off = start; off < end;) {
        read_rr(p, len, &off, &rr);  // as in the first pass, so it cannot fail now
        if (rr.class != CLASS_IN || rr.type != type || !dns_name_eq(rr.owner, target)) continue;
        if (type == DNS_TYPE_A) {
            uint32_t ip = rd32(p + rr.rd);
            int seen = 0;
            for (int i = 0; i < a->naddrs; i++) seen |= a->addrs[i] == ip;
            if (!seen && a->naddrs < DNS_MAX_ADDRS) a->addrs[a->naddrs++] = ip;
        } else if (type == DNS_TYPE_PTR && !a->ptr[0]) {
            int e;
            read_name(p, rr.rd + rr.rdlen, rr.rd, a->ptr, &e);
        }
    }
    return 0;
}

// Host test for the TLS 1.3 client (src/programs/lib/tls.c).
//
// tls_cases.h holds conversations recorded against two servers
// (tests/gen_tls_cases.py makes it): OpenSSL, and a scripted server
// written for these tests that can do everything a server may do in
// ways OpenSSL never chooses to, and everything a server must not do.
// Each case has the bytes the server sent, the bytes our client sent
// when it was recorded, and what the client should make of it all.
//
// Here the client is run again against the recorded server bytes. With
// the same random numbers it must send exactly the same bytes and reach
// the same end. Then the recordings are abused: delivered a byte at a
// time; cut short at every length; one bit flipped in every byte. The
// client must never crash (this runs under the address and
// undefined-behaviour sanitizers), and must never call a damaged
// conversation a good one.
//
//   test_tls                          run every case
//   test_tls N                        run case N, noisily
//   test_tls --live PORT SEED HOST REQUEST-HEX AGAIN-HEX
//       be the client for a server on 127.0.0.1 (used when recording)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls.h"
#include "sha256.h"

// A recorded conversation, as tls_cases.h has it: bytes are hex strings.
typedef struct {
    const char* name;
    const char* host;         // the name the client asks for; "" for none
    uint32_t seed;            // of the client's random numbers
    const char* request;      // what the client sends once connected
    const char* again;        // and what it sends after its first read gives data
    const char* server;       // every byte the server sent
    const char* client_hash;  // SHA-256 of every byte the client sent,
    uint32_t client_len;      //   and how many there were
    const char* data;         // what the client should read
    int connect, final;       // tls_connect's result; the result of the read that ended it
    int abuse;                // a good conversation, to be cut short and damaged
    uint32_t ignorable;       // how many of its bytes TLS says to ignore
} tls_record_t;

#ifndef NO_CASES
#include "tls_cases.h"
#else  // (gen_tls_cases.py builds the client this way, to make that file)
static const tls_record_t tls_records[] = { { "", "", 0, "", "", "", "", 0, "", 0, 0, 0, 0 } };
#endif

// The abuse below replays each conversation thousands of times, and
// every replay would do the same two X25519 computations, which are
// most of a handshake's work. The linker is told (--wrap) to send the
// client's calls here instead, where an answer worked out once is
// remembered. The real function has a test of its own (test_crypto).
int __real_x25519(uint8_t shared[32], const uint8_t secret[32], const uint8_t their_public[32]);
void __real_x25519_public(uint8_t public[32], const uint8_t secret[32]);

static struct {
    uint8_t secret[32], other[32], out[32];
    int result, used;
} memo[64];
static uint32_t memo_next;

int __wrap_x25519(uint8_t shared[32], const uint8_t secret[32], const uint8_t their_public[32]) {
    for (uint32_t i = 0; i < 64; i++) {
        if (memo[i].used && !memcmp(memo[i].secret, secret, 32) && !memcmp(memo[i].other, their_public, 32)) {
            memcpy(shared, memo[i].out, 32);
            return memo[i].result;
        }
    }
    uint32_t i = memo_next++ % 64;
    memcpy(memo[i].secret, secret, 32);
    memcpy(memo[i].other, their_public, 32);
    memo[i].result = __real_x25519(memo[i].out, secret, their_public);
    memo[i].used = 1;
    memcpy(shared, memo[i].out, 32);
    return memo[i].result;
}

void __wrap_x25519_public(uint8_t public[32], const uint8_t secret[32]) {
    static const uint8_t base[32] = { 9 };  // the public value is the secret times the curve's base point
    __wrap_x25519(public, secret, base);
}

// tls.c wants this from the system's library; here the host's will do.
int parse_ipv4(const char* s, uint32_t* out) {
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return -1;
    *out = ntohl(a.s_addr);
    return 0;
}

// The same with the hex undone.
typedef struct {
    const char *name, *host;
    uint32_t seed;
    uint8_t *request, *again, *server, *data;
    uint32_t request_len, again_len, server_len, data_len, client_len;
    uint8_t client_hash[32];
    int connect, final, abuse;
    uint32_t ignorable;
} tls_case_t;

static uint8_t* unhex(const char* hex, uint32_t* len) {
    uint32_t n = (uint32_t)strlen(hex) / 2;
    uint8_t* p = malloc(n + 1);
    for (uint32_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        p[i] = (uint8_t)v;
    }
    *len = n;
    return p;
}

static int failures, checks;

typedef struct {
    uint8_t* p;
    uint32_t n, cap;
} buf_t;

static void put(buf_t* b, const void* s, uint32_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
}

// The "random" numbers: a stream that depends only on the seed, so a
// recorded conversation can be replayed.
typedef struct {
    uint32_t seed, counter;
} rng_t;

static int fake_random(void* ctx, void* out, uint32_t n) {
    rng_t* g = ctx;
    uint8_t* p = out;
    while (n) {
        uint8_t block[32];
        uint32_t in[2] = { g->seed, g->counter++ };
        sha256(in, sizeof in, block);
        uint32_t take = n < 32 ? n : 32;
        memcpy(p, block, take);
        p += take;
        n -= take;
    }
    return 0;
}

// What a run of the client came to.
typedef struct {
    int connect, final;  // tls_connect's result; the result of the read that ended it
    buf_t data;          // what the server sent, as the application saw it
} result_t;

// The client program every case runs: connect, send the request, read
// to the end (sending `again` once the first data has come), say goodbye.
static void drive(const tls_io_t* io, const tls_case_t* c, result_t* res) {
    memset(res, 0, sizeof *res);
    tls_t* t = tls_new(io);
    res->connect = tls_connect(t, c->host[0] ? c->host : 0);
    if (res->connect == 0) {
        int r = c->request_len ? tls_write(t, c->request, c->request_len) : 0, first = 1;
        uint8_t chunk[1000];
        while (r >= 0 && (r = tls_read(t, chunk, sizeof chunk)) > 0) {
            put(&res->data, chunk, (uint32_t)r);
            if (first && c->again_len) r = tls_write(t, c->again, c->again_len);
            first = 0;
        }
        res->final = r;
        tls_close(t);
    } else if (tls_read(t, &res->final, 1) != res->connect || tls_write(t, "x", 1) != res->connect) {
        res->connect = -1;  // a dead connection must stay dead, with the same error
    }
    tls_free(t);
}

// ---- Replaying a recording ----

typedef struct {
    const uint8_t* in;
    uint32_t in_len, at;
    int mode;        // 0: all there is; 1: a byte at a time; 2: odd-sized pieces
    uint32_t lcg;
    buf_t out;
    rng_t rng;
} replay_t;

static int replay_read(void* ctx, void* buf, uint32_t n) {
    replay_t* r = ctx;
    uint32_t left = r->in_len - r->at;
    if (!left) return 0;
    if (n > left) n = left;
    if (r->mode == 1) n = 1;
    if (r->mode == 2) {
        r->lcg = r->lcg * 1103515245u + 12345u;
        uint32_t k = 1 + (r->lcg >> 16) % 37;
        if (n > k) n = k;
    }
    // Through a buffer of exactly n bytes, so the sanitizer sees any write past what was asked for.
    uint8_t* exact = malloc(n);
    memcpy(exact, r->in + r->at, n);
    memcpy(buf, exact, n);
    free(exact);
    r->at += n;
    return (int)n;
}

static int replay_write(void* ctx, const void* buf, uint32_t n) {
    put(&((replay_t*)ctx)->out, buf, n);
    return 0;
}

static int replay_random(void* ctx, void* out, uint32_t n) { return fake_random(&((replay_t*)ctx)->rng, out, n); }

static void replay(const tls_case_t* c, const uint8_t* server, uint32_t server_len, int mode, result_t* res, buf_t* sent) {
    replay_t r = { server, server_len, 0, mode, c->seed, { 0 }, { c->seed, 0 } };
    tls_io_t io = { replay_read, replay_write, replay_random, &r };
    drive(&io, c, res);
    *sent = r.out;
}

static int same(const result_t* res, const tls_case_t* c) {
    return res->connect == c->connect && res->final == c->final && res->data.n == c->data_len &&
           (c->data_len == 0 || memcmp(res->data.p, c->data, c->data_len) == 0);
}

static int sent_same(const buf_t* sent, const tls_case_t* c) {
    uint8_t h[32];
    sha256(sent->p, sent->n, h);
    return sent->n == c->client_len && memcmp(h, c->client_hash, 32) == 0;
}

static void check(int ok, const tls_case_t* c, const char* what, const result_t* res) {
    checks++;
    if (ok) return;
    failures++;
    if (failures > 10) return;
    printf("FAIL: %s: %s\n", c->name, what);
    if (res)
        printf("  connect %d (want %d), final %d (want %d), %u bytes (want %u)\n", res->connect, c->connect, res->final,
               c->final, res->data.n, c->data_len);
}

static void run_case(const tls_case_t* c, int verbose) {
    result_t res;
    buf_t sent;
    replay(c, c->server, c->server_len, 0, &res, &sent);
    if (verbose) {
        printf("%s\n  connect %d: %s\n  final %d: %s\n  %u bytes read, %u sent\n", c->name, res.connect,
               res.connect ? tls_strerror(res.connect) : "ok", res.final, res.final < 0 ? tls_strerror(res.final) : "ok",
               res.data.n, sent.n);
    }
    check(same(&res, c), c, "the outcome is the recorded one", &res);
    check(sent_same(&sent, c), c, "the client sends the recorded bytes", 0);
    free(res.data.p);
    free(sent.p);

    // However the bytes are cut up on arrival, nothing changes.
    for (int mode = 1; mode <= 2; mode++) {
        replay(c, c->server, c->server_len, mode, &res, &sent);
        check(same(&res, c) && sent_same(&sent, c), c,
              mode == 1 ? "the same, fed a byte at a time" : "the same, fed in odd pieces", &res);
        free(res.data.p);
        free(sent.p);
    }
    if (!c->abuse) return;

    // A good conversation, cut off after any number of bytes, is never
    // a good conversation: the client must end with an error (or, if
    // all the data and the goodbye got through, as recorded).
    uint8_t* copy = malloc(c->server_len);
    int ok = 1;
    for (uint32_t cut = 0; cut < c->server_len; cut++) {
        memcpy(copy, c->server, cut);
        replay(c, copy, cut, 0, &res, &sent);
        int clean = res.connect == 0 && res.final == 0;
        if (clean && !same(&res, c)) ok = 0;
        if (res.connect == 0 && res.data.n > c->data_len) ok = 0;
        if (res.data.n && memcmp(res.data.p, c->data, res.data.n) != 0) ok = 0;
        free(res.data.p);
        free(sent.p);
    }
    check(ok, c, "cut short anywhere, it is never taken for whole", 0);

    // One bit wrong in any byte the server sent: the client must notice.
    // (But for the few bytes TLS says to ignore: the version bytes in
    // the header of a record that is not encrypted.) Data handed over
    // before the damaged byte arrives is still the right data.
    ok = 1;
    uint32_t noticed = 0, ignored = 0;
    for (uint32_t i = 0; i < c->server_len; i++) {
        memcpy(copy, c->server, c->server_len);
        copy[i] ^= (uint8_t)(1 << (i % 8));
        replay(c, copy, c->server_len, 0, &res, &sent);
        int clean = res.connect == 0 && res.final == 0;
        if (clean) {
            ignored++;
            if (!same(&res, c)) ok = 0;
        } else {
            noticed++;
        }
        if (res.data.n > c->data_len || (res.data.n && memcmp(res.data.p, c->data, res.data.n) != 0)) ok = 0;
        free(res.data.p);
        free(sent.p);
    }
    if (ignored > c->ignorable) ok = 0;
    if (verbose) printf("  flipped a bit in each of %u bytes: %u noticed, %u ignorable\n", c->server_len, noticed, ignored);
    check(ok, c, "a flipped bit anywhere is noticed", 0);
    free(copy);
}

// ---- Being the client for a live server ----

static int sock_read(void* ctx, void* buf, uint32_t n) {
    ssize_t r = read(*(int*)ctx, buf, n);
    return r < 0 ? 0 : (int)r;  // an error on the socket is the end of the stream, as in a replay
}

static int sock_write(void* ctx, const void* buf, uint32_t n) {
    const uint8_t* p = buf;
    while (n) {
        ssize_t w = write(*(int*)ctx, p, n);
        if (w <= 0) break;  // the server has gone: carry on as a replay would
        p += w;
        n -= (uint32_t)w;
    }
    return 0;
}

typedef struct {
    int s;
    rng_t rng;
} live_t;

static int live_random(void* ctx, void* out, uint32_t n) { return fake_random(&((live_t*)ctx)->rng, out, n); }

static int live(int port, uint32_t seed, const char* host, const char* req_hex, const char* again_hex) {
    signal(SIGPIPE, SIG_IGN);
    live_t l = { socket(AF_INET, SOCK_STREAM, 0), { seed, 0 } };
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(l.s, (struct sockaddr*)&a, sizeof a) != 0) {
        perror("connect");
        return 2;
    }
    tls_case_t c = { .name = "live", .host = host, .seed = seed };
    c.request = unhex(req_hex, &c.request_len);
    c.again = unhex(again_hex, &c.again_len);
    tls_io_t io = { sock_read, sock_write, live_random, &l };  // (the socket is first in live_t)
    result_t res;
    drive(&io, &c, &res);
    // Hang up politely: say we are done, then wait for the server to be,
    // so nothing either side sent is thrown away by a reset.
    shutdown(l.s, SHUT_WR);
    uint8_t rest[4096];
    while (read(l.s, rest, sizeof rest) > 0) {}
    close(l.s);
    printf("connect=%d final=%d data=", res.connect, res.final);
    for (uint32_t i = 0; i < res.data.n; i++) printf("%02x", res.data.p[i]);
    printf("\n");
    free(c.request);
    free(c.again);
    free(res.data.p);
    return 0;
}

// The key schedule's one building block, against RFC 8448's worked
// example ("Simple 1-RTT Handshake"): the server handshake traffic
// secret given there, and the key, iv and finished key it derives. (The
// example uses AES-128, so its key is 16 bytes; the function does not
// care.)
static int hexeq(const uint8_t* got, const char* want) {
    for (uint32_t i = 0; want[2 * i]; i++) {
        unsigned v;
        sscanf(want + 2 * i, "%2x", &v);
        if (got[i] != v) return 0;
    }
    return 1;
}

static void test_expand_label(void) {
    static const uint8_t s_hs[32] = {
        0xb6, 0x7b, 0x7d, 0x69, 0x0c, 0xc1, 0x6c, 0x4e, 0x75, 0xe5, 0x42, 0x13, 0xcb, 0x2d, 0x37, 0xb4,
        0xe9, 0xc9, 0x12, 0xbc, 0xde, 0xd9, 0x10, 0x5d, 0x42, 0xbe, 0xfd, 0x59, 0xd3, 0x91, 0xad, 0x38,
    };
    uint8_t out[32];
    int ok = 1;
    tls13_expand_label(s_hs, "key", 0, 0, out, 16);
    ok &= hexeq(out, "3fce516009c21727d0f2e4e86ee403bc");
    tls13_expand_label(s_hs, "iv", 0, 0, out, 12);
    ok &= hexeq(out, "5d313eb2671276ee13000b30");
    tls13_expand_label(s_hs, "finished", 0, 0, out, 32);
    ok &= hexeq(out, "008d3b66f816ea559f96b537e885c31fc068bf492c652f01f288a1d8cdc19fc8");
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: HKDF-Expand-Label does not match RFC 8448\n");
    }
}

int main(int argc, char** argv) {
    if (argc == 7 && !strcmp(argv[1], "--live"))
        return live(atoi(argv[2]), (uint32_t)strtoul(argv[3], 0, 10), argv[4], argv[5], argv[6]);
    int n = (int)(sizeof tls_records / sizeof *tls_records), only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0) test_expand_label();
    for (int i = 0; i < n; i++) {
        if (only >= 0 && i != only) continue;
        const tls_record_t* r = &tls_records[i];
        tls_case_t c = { .name = r->name, .host = r->host, .seed = r->seed, .client_len = r->client_len,
                         .connect = r->connect, .final = r->final, .abuse = r->abuse, .ignorable = r->ignorable };
        uint32_t hash_len;
        uint8_t* hash = unhex(r->client_hash, &hash_len);
        memcpy(c.client_hash, hash, 32);
        c.request = unhex(r->request, &c.request_len);
        c.again = unhex(r->again, &c.again_len);
        c.server = unhex(r->server, &c.server_len);
        c.data = unhex(r->data, &c.data_len);
        run_case(&c, only >= 0);
        free(hash);
        free(c.request);
        free(c.again);
        free(c.server);
        free(c.data);
    }
    printf("test_tls: %d of %d checks pass (%d recorded conversations)\n", checks - failures, checks, n);
    return failures ? 1 : 0;
}

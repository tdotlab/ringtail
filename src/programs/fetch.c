// fetch [-i] [-o FILE] URL    get a page over HTTP or HTTPS
//
// URL is [http://|https://]HOST[:PORT][/PATH]. The body goes to the
// screen, or to FILE with -o; -i prints the response headers first.
// Redirects are followed, five at most, from either scheme to the
// other. An error status (400 and up) is reported and its body left
// unread.
//
// https is TLS 1.3 (lib/tls.h). The connection is encrypted, but the
// server's certificate is NOT checked yet: nothing proves who is at the
// other end, and fetch says so on every such connection.
#include "ulib.h"
#include "dns.h"
#include "dial.h"
#include "tls.h"

#define IDLE_MS 30000        // give up when the server goes quiet this long
#define MAX_REDIRECTS 5

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: fetch [-i] [-o FILE] URL\n");
    return 2;
}

static char host[256], path[1024];
static uint32_t port;
static int secure;           // https
static char hdr[8192];       // the response headers, then the first of the body
static char buf[4096];

static int prefix(const char* s, const char* p) {  // p is lower case
    for (; *p; s++, p++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (c != *p) return 0;
    }
    return 1;
}

// Some other scheme ("ftp://..."): letters, then "://".
static int strstr_scheme(const char* s) {
    const char* p = s;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
    return p > s && p[0] == ':' && p[1] == '/' && p[2] == '/';
}

// A number at the start of s, ended by anything but a digit: 0, or -1.
static int parse_uint_prefix(const char* s, int* out) {
    uint32_t v = 0;
    const char* p = s;
    for (; *p >= '0' && *p <= '9' && p - s < 10; p++) v = v * 10 + (uint32_t)(*p - '0');
    if (p == s || (*p >= '0' && *p <= '9')) return -1;
    *out = (int)v;
    return 0;
}

// Take apart an absolute URL into host, port and path. 0, or a message.
static const char* parse_url(const char* url) {
    int https = prefix(url, "https://");
    if (https) url += 8;
    else if (prefix(url, "http://")) url += 7;
    else if (strstr_scheme(url)) return "only http and https URLs are supported";
    const char* end = url;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    const char* colon = 0;
    for (const char* p = url; p < end; p++)
        if (*p == ':') colon = p;
    const char* hend = colon ? colon : end;
    if (hend == url || hend - url >= (int)sizeof host) return "bad host name";
    memcpy(host, url, (size_t)(hend - url));
    host[hend - url] = 0;
    secure = https;
    port = https ? 443 : 80;
    if (colon) {
        char num[8];
        if (end - colon - 1 < 1 || end - colon - 1 >= (int)sizeof num) return "bad port";
        memcpy(num, colon + 1, (size_t)(end - colon - 1));
        num[end - colon - 1] = 0;
        if (parse_uint(num, &port) || port == 0 || port > 65535) return "bad port";
    }
    const char* rest = *end == '/' ? end : "/";
    uint32_t n = 0;
    if (*end != '/' && *end == '?') path[n++] = '/';
    if (*end == '?') rest = end;
    for (; *rest && *rest != '#' && n < sizeof path - 1; rest++) path[n++] = *rest;
    if (*rest && *rest != '#') return "URL too long";
    path[n] = 0;
    return 0;
}

// Follow a Location: an absolute URL, //host/path, /path, or relative
// to the page that sent it. All but the first keep the page's scheme.
static const char* follow(const char* loc) {
    static char url[1310];
    const char* scheme = secure ? "https" : "http";
    if (loc[0] == '/' && loc[1] == '/') {
        snprintf(url, sizeof url, "%s:%s", scheme, loc);
        return parse_url(url);
    }
    if (prefix(loc, "http://") || prefix(loc, "https://") || strstr_scheme(loc)) return parse_url(loc);
    char hp[310];
    snprintf(hp, sizeof hp, "%s://%s:%u", scheme, host, port);
    if (loc[0] == '/') snprintf(url, sizeof url, "%s%s", hp, loc);
    else {
        char* q = strchr(path, '?');
        if (q) *q = 0;
        char* slash = strrchr(path, '/');
        slash[1] = 0;
        snprintf(url, sizeof url, "%s%s%s", hp, path, loc);
    }
    return parse_url(url);
}

// The connection: a socket, and for https a TLS session on top of it,
// which reads and writes the socket through the three functions below.
static int sock = -1;
static tls_t* tls;

// Some of what the server sent, waiting no longer than IDLE_MS for it.
static int sock_read(void* ctx, void* p, uint32_t n) {
    (void)ctx;
    pollfd_t pf = { sock, POLLIN, 0 };
    int r = sys_pollfd(&pf, 1, IDLE_MS);
    if (r < 0) return r;
    if (r == 0) return -ETIMEDOUT;
    return sys_read(sock, p, n);
}

static int sock_write(void* ctx, const void* p, uint32_t n) {
    (void)ctx;
    for (uint32_t done = 0; done < n;) {
        int r = sys_write(sock, (const char*)p + done, n - done);
        if (r < 0) return r;
        done += (uint32_t)r;
    }
    return 0;
}

static int sock_random(void* ctx, void* p, uint32_t n) {
    (void)ctx;
    int r = sys_random(p, n);
    return r < 0 ? r : 0;
}

// Read some of the response: how many bytes, 0 at its end, or an error
// (tls_strerror has the words for it, either way).
static int read_some(char* p, uint32_t n) {
    return tls ? tls_read(tls, p, n) : sock_read(0, p, n);
}

static int write_all(const char* p, uint32_t n) {
    if (!tls) return sock_write(0, p, n);
    int r = tls_write(tls, p, n);
    return r < 0 ? r : 0;
}

static void hang_up(void) {
    if (tls) {
        tls_close(tls);  // tells the server nothing was cut short
        tls_free(tls);
        tls = 0;
    }
    sys_close(sock);
    sock = -1;
}

// The handshake, and the warning that goes with it for now.
static int start_tls(void) {
    static const tls_io_t io = { sock_read, sock_write, sock_random, 0 };
    static char warned[sizeof host];
    tls = tls_new(&io);
    int r = tls ? tls_connect(tls, host) : TLS_E_NOMEM;
    if (r < 0) return r;
    if (!tls_peer_verified(tls) && strcmp(warned, host)) {
        dprintf(STDERR_FILENO,
                "fetch: warning: the certificate of %s was NOT checked. The connection is\n"
                "encrypted, but nothing proves who is at the other end.\n", host);
        memcpy(warned, host, sizeof warned);
    }
    return 0;
}

// The value of header `name` (lower case, with the colon), or 0.
static char* header(char* h, const char* name) {
    for (char* line = strchr(h, '\n'); line; line = strchr(line, '\n')) {
        line++;
        if (!prefix(line, name)) continue;
        char* v = line + strlen(name);
        while (*v == ' ' || *v == '\t') v++;
        return v;
    }
    return 0;
}

static void cut_line(char* v) {
    while (*v && *v != '\r' && *v != '\n') v++;
    *v = 0;
}

// A chunked body (RFC 9112 section 7.1): pieces, each after a line with
// its length in hex, then a piece of length 0 and maybe some trailing
// header lines, then an empty line.
enum { CK_SIZE, CK_LINE, CK_DATA, CK_CR, CK_LF, CK_TRAILER, CK_TRAILER_LINE, CK_DONE, CK_BAD };
static struct {
    int state, digits;
    uint32_t left;  // of the piece being read
} ck;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Takes the framing out of the n bytes at p, in place: how many bytes of
// the body are now at p (it may be none). Call it with each part of the
// response as it comes; ck.state is CK_DONE once the body has ended, or
// CK_BAD when the framing is wrong (what came before it still counts).
static int dechunk(char* p, int n) {
    int out = 0;
    for (int i = 0; i < n && ck.state < CK_DONE;) {
        char c = p[i];
        switch (ck.state) {
        case CK_SIZE: {
            int d = hex_digit(c);
            if (d >= 0) {
                if (++ck.digits > 8) ck.state = CK_BAD;  // more than fits, and more than anyone sends
                ck.left = ck.left << 4 | (uint32_t)d;
                i++;
            } else ck.state = ck.digits ? CK_LINE : CK_BAD;
            break;
        }
        case CK_LINE:  // the rest of the length's line: extensions, which nobody uses
            i++;
            if (c != '\n') break;
            ck.state = ck.left ? CK_DATA : CK_TRAILER;
            ck.digits = 0;
            break;
        case CK_DATA: {
            uint32_t take = (uint32_t)(n - i) < ck.left ? (uint32_t)(n - i) : ck.left;
            memmove(p + out, p + i, take);
            out += (int)take;
            i += (int)take;
            ck.left -= take;
            if (!ck.left) ck.state = CK_CR;
            break;
        }
        case CK_CR:  // the line end after a piece
            if (c == '\r') i++;
            ck.state = CK_LF;
            break;
        case CK_LF:
            i++;
            ck.state = c == '\n' ? CK_SIZE : CK_BAD;
            break;
        case CK_TRAILER:  // at the start of a line: an empty one is the end
            i++;
            if (c == '\n') ck.state = CK_DONE;
            else if (c != '\r') ck.state = CK_TRAILER_LINE;
            break;
        case CK_TRAILER_LINE:
            i++;
            if (c == '\n') ck.state = CK_TRAILER;
            break;
        }
    }
    return out;
}

int main(int argc, char** argv) {
    int show = 0;
    const char* out = 0;
    const char* url = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) show = 1;
        else if (!strcmp(argv[i], "-o")) {
            if (++i == argc) return usage();
            out = argv[i];
        } else if (argv[i][0] == '-' || url) return usage();
        else url = argv[i];
    }
    if (!url) return usage();
    const char* err = parse_url(url);
    if (err) {
        dprintf(STDERR_FILENO, "fetch: %s\n", err);
        return 1;
    }

    int status = 0, redirects = 0;
    for (;;) {
        uint32_t ip = 0;
        sock = dial(host, (int)port, 10000, &ip);
        if (sock < 0) {
            if (sock == -EINTR) return 1;
            if (ip) dprintf(STDERR_FILENO, "fetch: " IPV4_FMT " port %u: %s\n", IPV4_ARGS(ip), port, dns_strerror(sock));
            else dprintf(STDERR_FILENO, "fetch: %s: %s\n", host, dns_strerror(sock));
            return 1;
        }
        int r;
        if (secure && (r = start_tls()) < 0) {
            if (r != -EINTR) dprintf(STDERR_FILENO, "fetch: %s: %s\n", host, tls_strerror(r));
            return 1;
        }
        // HTTP/1.1, because plenty of servers now turn 1.0 away, but one
        // request per connection: the server is asked to close after it.
        int n = port == (secure ? 443u : 80u) ? snprintf(buf, sizeof buf,
                                                         "GET %s HTTP/1.1\r\nHost: %s\r\n", path, host)
                                              : snprintf(buf, sizeof buf,
                                                         "GET %s HTTP/1.1\r\nHost: %s:%u\r\n", path, host, port);
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "User-Agent: fetch (RingTail OS)\r\nAccept: */*\r\nConnection: close\r\n\r\n");
        r = write_all(buf, (uint32_t)n);
        if (r < 0) {
            dprintf(STDERR_FILENO, "fetch: %s\n", tls_strerror(r));
            return 1;
        }

        // The headers end at the first empty line. A status in the 100s
        // is an aside ("103 Early Hints"); the real response follows it.
        uint32_t have = 0, body_at;
        hdr[0] = 0;
        for (;;) {
            body_at = 0;
            for (uint32_t i = 0; i + 1 < have && !body_at; i++) {
                if (hdr[i] != '\n') continue;
                if (hdr[i + 1] == '\n') body_at = i + 2;
                else if (hdr[i + 1] == '\r' && i + 2 < have && hdr[i + 2] == '\n') body_at = i + 3;
            }
            if (body_at) {
                if (!prefix(hdr, "http/1.") || hdr[8] != ' ' || parse_uint_prefix(hdr + 9, &status)) {
                    dprintf(STDERR_FILENO, "fetch: not an HTTP response\n");
                    return 1;
                }
                if (status < 100 || status > 199 || status == 101) break;
                if (show) sys_write(STDOUT_FILENO, hdr, body_at);
                have -= body_at;
                memmove(hdr, hdr + body_at, have);
                hdr[have] = 0;
                continue;
            }
            if (have == sizeof hdr - 1) {
                dprintf(STDERR_FILENO, "fetch: response headers too long\n");
                return 1;
            }
            r = read_some(hdr + have, sizeof hdr - 1 - have);
            if (r <= 0) {
                if (r == -EINTR) return 1;
                dprintf(STDERR_FILENO, "fetch: %s\n", r ? tls_strerror(r) : "connection closed before a response");
                return 1;
            }
            have += (uint32_t)r;
            hdr[have] = 0;
        }
        // hdr holds the header block as a string (its last byte is
        // replaced by a 0); the body's first bytes, if any, are kept apart.
        static char early[sizeof hdr];
        uint32_t nearly = have - body_at;
        memcpy(early, hdr + body_at, nearly);
        hdr[body_at] = 0;
        if (show) sys_write(STDOUT_FILENO, hdr, body_at);

        char* loc = header(hdr, "location:");
        if (loc && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            cut_line(loc);
            hang_up();
            if (++redirects > MAX_REDIRECTS) {
                dprintf(STDERR_FILENO, "fetch: too many redirects\n");
                return 1;
            }
            if ((err = follow(loc))) {
                dprintf(STDERR_FILENO, "fetch: redirect to %s: %s\n", loc, err);
                return 1;
            }
            continue;
        }
        if (status >= 400) {
            char* reason = hdr + 9;
            cut_line(reason);
            dprintf(STDERR_FILENO, "fetch: HTTP %s\n", reason);
            return 1;
        }
        // Where the body ends: after a stated length, at the last chunk,
        // or, with neither, when the server closes the connection.
        uint32_t want = 0;
        int sized = 0, chunked = 0;
        char* te = header(hdr, "transfer-encoding:");
        char* cl = header(hdr, "content-length:");
        if (status == 204 || status == 304) sized = 1;  // these have no body at all
        else if (te) {
            cut_line(te);
            const char* rest = te + 7;
            while (prefix(te, "chunked") && (*rest == ' ' || *rest == '\t')) rest++;
            if (!prefix(te, "chunked") || *rest) {
                dprintf(STDERR_FILENO, "fetch: transfer encoding \"%s\" is not supported\n", te);
                return 1;
            }
            chunked = 1;
        } else if (cl) sized = !parse_uint_prefix(cl, (int*)&want);

        int fd = STDOUT_FILENO;
        if (out) {
            fd = sys_open(out, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                dprintf(STDERR_FILENO, "fetch: %s: %s\n", out, strerror(fd));
                return 1;
            }
        }
        uint32_t got = 0;
        int fail = 0;
        char* p = early;
        n = (int)nearly;
        memset(&ck, 0, sizeof ck);
        for (;;) {
            if (chunked && n > 0) n = dechunk(p, n);
            if (n > 0) {
                if (sized && got + (uint32_t)n > want) n = (int)(want - got);
                int w = sys_write(fd, p, (uint32_t)n);
                if (w < 0) {
                    dprintf(STDERR_FILENO, "fetch: %s: %s\n", out ? out : "output", strerror(w));
                    fail = 1;
                    break;
                }
                got += (uint32_t)n;
            }
            if (chunked && ck.state == CK_BAD) {
                dprintf(STDERR_FILENO, "\nfetch: the chunked body is malformed\n");
                fail = 1;
                break;
            }
            if (sized ? got == want : chunked && ck.state == CK_DONE) break;
            n = read_some(buf, sizeof buf);
            p = buf;
            if (n > 0) continue;
            // The end, one way or another. TLS_E_EOF is a TLS connection
            // that stopped without the server saying it was done: for a
            // body with no other end that may be someone cutting it short.
            if (n == 0 || n == TLS_E_EOF) {
                if (sized) dprintf(STDERR_FILENO, "\nfetch: connection closed after %u of %u bytes\n", got, want);
                else if (chunked) dprintf(STDERR_FILENO, "\nfetch: connection closed before the end of the body\n");
                else if (n) dprintf(STDERR_FILENO, "\nfetch: the connection stopped before the server said it was done; "
                                                   "the page may be cut short\n");
                else break;
            } else if (n != -EINTR) dprintf(STDERR_FILENO, "\nfetch: %s\n", tls_strerror(n));
            fail = 1;
            break;
        }
        hang_up();
        if (out) {
            sys_close(fd);
            if (fail) sys_delete(out);
            else dprintf(STDERR_FILENO, "saved %u bytes to %s\n", got, out);
        }
        return fail;
    }
}

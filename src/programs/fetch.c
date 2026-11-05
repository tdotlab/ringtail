// fetch [-i] [-o FILE] URL    get a page over HTTP
//
// URL is [http://]HOST[:PORT][/PATH]. The body goes to the screen, or
// to FILE with -o; -i prints the response headers first. Redirects
// are followed, five at most. There is no TLS, so https is refused.
// An error status (400 and up) is reported and its body left unread.
#include "ulib.h"
#include "dns.h"
#include "dial.h"

#define IDLE_MS 30000        // give up when the server goes quiet this long
#define MAX_REDIRECTS 5

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: fetch [-i] [-o FILE] URL\n");
    return 2;
}

static char host[256], path[1024];
static uint32_t port;
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
    if (prefix(url, "https://")) return "https is not supported (no TLS)";
    if (prefix(url, "http://")) url += 7;
    else if (strstr_scheme(url)) return "only http URLs are supported";
    const char* end = url;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    const char* colon = 0;
    for (const char* p = url; p < end; p++)
        if (*p == ':') colon = p;
    const char* hend = colon ? colon : end;
    if (hend == url || hend - url >= (int)sizeof host) return "bad host name";
    memcpy(host, url, (size_t)(hend - url));
    host[hend - url] = 0;
    port = 80;
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
// to the page that sent it.
static const char* follow(const char* loc) {
    static char url[1300];
    if (loc[0] == '/' && loc[1] == '/') {
        snprintf(url, sizeof url, "http:%s", loc);
        return parse_url(url);
    }
    if (prefix(loc, "http://") || prefix(loc, "https://") || strstr_scheme(loc)) return parse_url(loc);
    char hp[300];
    if (port == 80) snprintf(hp, sizeof hp, "%s", host);
    else snprintf(hp, sizeof hp, "%s:%u", host, port);
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

// Read some of the response, waiting no longer than IDLE_MS for it.
static int read_some(int s, char* p, uint32_t n) {
    pollfd_t pf = { s, POLLIN, 0 };
    int r = sys_pollfd(&pf, 1, IDLE_MS);
    if (r < 0) return r;
    if (r == 0) return -ETIMEDOUT;
    return sys_read(s, p, n);
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

    int s = -1, status = 0, redirects = 0;
    uint32_t have = 0, body_at = 0;
    for (;;) {
        uint32_t ip = 0;
        s = dial(host, (int)port, 10000, &ip);
        if (s < 0) {
            if (s == -EINTR) return 1;
            if (ip) dprintf(STDERR_FILENO, "fetch: " IPV4_FMT " port %u: %s\n", IPV4_ARGS(ip), port, dns_strerror(s));
            else dprintf(STDERR_FILENO, "fetch: %s: %s\n", host, dns_strerror(s));
            return 1;
        }
        int n = port == 80 ? snprintf(buf, sizeof buf,
                                      "GET %s HTTP/1.0\r\nHost: %s\r\n", path, host)
                           : snprintf(buf, sizeof buf,
                                      "GET %s HTTP/1.0\r\nHost: %s:%u\r\n", path, host, port);
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "User-Agent: fetch (RingTail OS)\r\nAccept: */*\r\nConnection: close\r\n\r\n");
        int r = sys_write(s, buf, (uint32_t)n);
        if (r < 0) {
            dprintf(STDERR_FILENO, "fetch: %s\n", strerror(r));
            return 1;
        }

        // The headers end at the first empty line.
        have = 0;
        body_at = 0;
        while (!body_at) {
            if (have == sizeof hdr - 1) {
                dprintf(STDERR_FILENO, "fetch: response headers too long\n");
                return 1;
            }
            r = read_some(s, hdr + have, sizeof hdr - 1 - have);
            if (r <= 0) {
                if (r == -EINTR) return 1;
                dprintf(STDERR_FILENO, "fetch: %s\n", r ? strerror(r) : "connection closed before a response");
                return 1;
            }
            have += (uint32_t)r;
            hdr[have] = 0;
            for (uint32_t i = 0; i + 1 < have; i++) {
                if (hdr[i] == '\n' && hdr[i + 1] == '\n') { body_at = i + 2; break; }
                if (hdr[i] == '\n' && hdr[i + 1] == '\r' && i + 2 < have && hdr[i + 2] == '\n') {
                    body_at = i + 3;
                    break;
                }
            }
        }
        // hdr holds the header block as a string (its last byte is
        // replaced by a 0); the body's first bytes, if any, are kept apart.
        static char early[sizeof hdr];
        uint32_t nearly = have - body_at;
        memcpy(early, hdr + body_at, nearly);
        hdr[body_at] = 0;

        if (!prefix(hdr, "http/1.") || hdr[8] != ' ' || parse_uint_prefix(hdr + 9, &status)) {
            dprintf(STDERR_FILENO, "fetch: not an HTTP response\n");
            return 1;
        }
        if (show) sys_write(STDOUT_FILENO, hdr, body_at);

        char* loc = header(hdr, "location:");
        if (loc && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            cut_line(loc);
            sys_close(s);
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
        char* te = header(hdr, "transfer-encoding:");
        if (te && prefix(te, "chunked")) {
            dprintf(STDERR_FILENO, "fetch: chunked responses are not supported\n");
            return 1;
        }
        uint32_t want = 0;
        char* cl = header(hdr, "content-length:");
        int sized = cl && !parse_uint_prefix(cl, (int*)&want);

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
        const char* p = early;
        n = (int)nearly;
        for (;;) {
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
            if (sized && got == want) break;
            n = read_some(s, buf, sizeof buf);
            p = buf;
            if (n == 0) break;
            if (n < 0) {
                if (n != -EINTR) dprintf(STDERR_FILENO, "\nfetch: %s\n", strerror(n));
                fail = 1;
                break;
            }
        }
        if (!fail && sized && got < want) {
            dprintf(STDERR_FILENO, "\nfetch: connection closed after %u of %u bytes\n", got, want);
            fail = 1;
        }
        sys_close(s);
        if (out) {
            sys_close(fd);
            if (fail) sys_delete(out);
            else dprintf(STDERR_FILENO, "saved %u bytes to %s\n", got, out);
        }
        return fail;
    }
}

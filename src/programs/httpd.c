// httpd [-p PORT] [-q] [DIR]    serve the files in DIR (default: here) over HTTP
//
// GET and HEAD, HTTP/1.0, one connection at a time. A directory gives
// its index.html, or else a listing. Names with .. in them are refused,
// so nothing outside DIR can be reached. A client that does not finish
// its request within 10 s is dropped. Every request is logged to the
// screen unless -q; Ctrl+C stops the server.
#include "ulib.h"
#include "dns.h"

#define PORT_DEFAULT 8080
#define HEADER_MS 10000
#define REQ_MAX 4096

static int usage(void) {
    dprintf(STDERR_FILENO, "usage: httpd [-p PORT] [-q] [DIR]\n");
    return 2;
}

static const char* root = ".";
static int quiet;
static char req[REQ_MAX + 1];
static char out[2048];  // output not yet sent, for pages built as they go
static uint32_t outn;
static int out_bad;     // a write failed: the client is gone

static void flush(int c) {
    if (outn && !out_bad && sys_write(c, out, outn) != (int)outn) out_bad = 1;
    outn = 0;
}

static void emit(int c, const char* s, uint32_t n) {
    while (n) {
        if (outn == sizeof out) flush(c);
        uint32_t k = sizeof out - outn < n ? (uint32_t)sizeof out - outn : n;
        memcpy(out + outn, s, k);
        outn += k;
        s += k;
        n -= k;
    }
}

static void emits(int c, const char* s) { emit(c, s, (uint32_t)strlen(s)); }

static void emit_escaped(int c, const char* s, int as_url) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (as_url) {
            int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                       ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (safe) emit(c, (const char*)&ch, 1);
            else { char e[3] = { '%', hex[ch >> 4], hex[ch & 15] }; emit(c, e, 3); }
        } else if (ch == '&') emits(c, "&amp;");
        else if (ch == '<') emits(c, "&lt;");
        else if (ch == '>') emits(c, "&gt;");
        else if (ch == '"') emits(c, "&quot;");
        else emit(c, (const char*)&ch, 1);
    }
}

static const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    default: return "Internal Server Error";
    }
}

static const char* mime(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return "application/octet-stream";
    static const struct { const char* ext; const char* type; } t[] = {
        { ".html", "text/html" }, { ".htm", "text/html" }, { ".css", "text/css" },
        { ".txt", "text/plain" }, { ".md", "text/plain" }, { ".c", "text/plain" }, { ".h", "text/plain" },
        { ".js", "text/javascript" }, { ".json", "application/json" }, { ".png", "image/png" },
        { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" }, { ".pdf", "application/pdf" },
    };
    for (uint32_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcasecmp(dot, t[i].ext)) return t[i].type;
    return "application/octet-stream";
}

// The status line and headers. length < 0: not known, the body just ends.
static void head(int c, int status, const char* type, int64_t length, const char* location) {
    char line[160];
    snprintf(line, sizeof line, "HTTP/1.0 %d %s\r\nServer: httpd (RingTail OS)\r\nConnection: close\r\n", status,
             reason(status));
    emits(c, line);
    if (type) {
        snprintf(line, sizeof line, "Content-Type: %s%s\r\n", type, !strncmp(type, "text/", 5) ? "; charset=utf-8" : "");
        emits(c, line);
    }
    if (length >= 0) {
        snprintf(line, sizeof line, "Content-Length: %u\r\n", (uint32_t)length);
        emits(c, line);
    }
    if (location) {
        emits(c, "Location: ");
        emits(c, location);
        emits(c, "\r\n");
    }
    if (status == 405) emits(c, "Allow: GET, HEAD\r\n");
    emits(c, "\r\n");
}

// A short plain answer for an error.
static void fail(int c, int status, int send_body) {
    char body[64];
    int n = snprintf(body, sizeof body, "%d %s\n", status, reason(status));
    head(c, status, "text/plain", n, 0);
    if (send_body) emit(c, body, (uint32_t)n);
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Turn the request's path into a relative one: percent-decoded, with empty
// and "." parts dropped. 0, or the status to answer with. *slash says it
// ended in "/", and *url is the path as the client wrote it.
static int clean_path(const char* target, char* rel, uint32_t size, int* slash, char* url, uint32_t usize) {
    uint32_t n = 0, u = 0;
    while (target[u] && target[u] != '?' && target[u] != '#') u++;
    if (u >= usize) return 414;
    memcpy(url, target, u);
    url[u] = 0;
    *slash = u > 0 && url[u - 1] == '/';
    rel[0] = 0;
    const char* p = url;
    while (*p) {
        while (*p == '/') p++;
        char seg[NAME_MAX + 1];
        uint32_t k = 0;
        while (*p && *p != '/') {
            int ch = (unsigned char)*p;
            if (ch == '%') {
                int a = hexval(p[1]), b = a < 0 ? -1 : hexval(p[2]);
                if (b < 0) return 400;
                ch = a * 16 + b;
                p += 2;
            }
            if (ch < 0x20 || ch == 0x7F || ch == '\\' || ch == '/' || ch == ':') return ch == '/' || ch == ':' || ch == '\\' ? 403 : 400;
            if (k >= NAME_MAX) return 414;
            seg[k++] = (char)ch;
            p++;
        }
        seg[k] = 0;
        if (!k || !strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) return 403;
        if (n + k + 2 > size) return 414;
        if (n) rel[n++] = '/';
        memcpy(rel + n, seg, k + 1);
        n += k;
    }
    return 0;
}

static void listing(int c, const char* dir, const char* url, int has_parent, int send_body) {
    head(c, 200, "text/html", -1, 0);
    if (!send_body) return;
    emits(c, "<!DOCTYPE html>\n<title>Index of ");
    emit_escaped(c, url, 0);
    emits(c, "</title>\n<h1>Index of ");
    emit_escaped(c, url, 0);
    emits(c, "</h1>\n<ul>\n");
    if (has_parent) emits(c, "<li><a href=\"../\">../</a>\n");
    dirent_t d;
    int cursor = 0;
    while (sys_readdir(dir, &cursor, &d) == 1) {
        emits(c, "<li><a href=\"");
        emit_escaped(c, d.name, 1);
        if (d.flags & DIRENT_DIR) emits(c, "/");
        emits(c, "\">");
        emit_escaped(c, d.name, 0);
        if (d.flags & DIRENT_DIR) emits(c, "/");
        emits(c, "</a>");
        if (!(d.flags & DIRENT_DIR)) {
            char sz[24];
            snprintf(sz, sizeof sz, " (%u bytes)", d.size);
            emits(c, sz);
        }
        emits(c, "\n");
    }
    emits(c, "</ul>\n");
}

// Answer one request. Returns the status sent, and *sent the body's size.
static int serve(int c, char* method_line, uint32_t* sent) {
    *sent = 0;
    char* method = method_line;
    char* target = strchr(method, ' ');
    if (!target) { fail(c, 400, 1); return 400; }
    *target++ = 0;
    char* version = strchr(target, ' ');
    if (!version) { fail(c, 400, 1); return 400; }
    *version++ = 0;
    if (strncmp(version, "HTTP/", 5) || target[0] != '/') { fail(c, 400, 1); return 400; }
    int get = !strcmp(method, "GET"), is_head = !strcmp(method, "HEAD");
    if (!get && !is_head) { fail(c, 405, 1); return 405; }

    char rel[PATH_MAX], url[PATH_MAX], path[PATH_MAX];
    int slash;
    int st = clean_path(target, rel, sizeof rel, &slash, url, sizeof url);
    if (st) { fail(c, st, get); return st; }
    int n = rel[0] ? snprintf(path, sizeof path, "%s/%s", root, rel) : snprintf(path, sizeof path, "%s", root);
    if (n >= (int)sizeof path) { fail(c, 414, get); return 414; }

    dirent_t d;
    if (sys_stat(path, &d) < 0) { fail(c, 404, get); return 404; }
    if (d.flags & DIRENT_DIR) {
        if (!slash && rel[0]) {
            char where[PATH_MAX + 2];
            snprintf(where, sizeof where, "%s/", url);
            head(c, 301, "text/plain", 0, where);
            return 301;
        }
        char index[PATH_MAX + 16];
        if (snprintf(index, sizeof index, "%s/index.html", path) < (int)sizeof index && sys_stat(index, &d) == 0 &&
            !(d.flags & DIRENT_DIR)) {
            strlcpy(path, index, sizeof path);
        } else {
            listing(c, path, url, rel[0] != 0, get);
            return 200;
        }
    }
    int f = sys_open(path, O_RDONLY);
    if (f < 0) { fail(c, 404, get); return 404; }
    head(c, 200, mime(path), d.size, 0);
    if (get) {
        flush(c);
        static char buf[4096];
        int r;
        while (!out_bad && (r = sys_read(f, buf, sizeof buf)) > 0) {
            if (sys_write(c, buf, (uint32_t)r) != r) out_bad = 1;
            else *sent += (uint32_t)r;
        }
    }
    sys_close(f);
    return 200;
}

// Whether the request so far has reached its blank line.
static int complete(uint32_t have) {
    for (uint32_t i = 0; i + 1 < have; i++) {
        if (req[i] != '\n') continue;
        if (req[i + 1] == '\n' || (req[i + 1] == '\r' && i + 2 < have && req[i + 2] == '\n')) return 1;
    }
    return 0;
}

// Read the request up to its blank line. The length of the first line is
// returned (its end marked with a 0), or a negative errno or -1.
static int read_request(int c) {
    uint32_t have = 0;
    for (;;) {
        pollfd_t pf = { c, POLLIN, 0 };
        int r = sys_pollfd(&pf, 1, HEADER_MS);
        if (r < 0) return r;
        if (r == 0) return -ETIMEDOUT;
        r = sys_read(c, req + have, REQ_MAX - have);
        if (r < 0) return r;
        if (r == 0) {  // the client is done sending: answer what there is
            if (!have) return -1;
            break;
        }
        have += (uint32_t)r;
        req[have] = 0;
        if (complete(have)) break;
        if (have == REQ_MAX) return -E2BIG;
    }
    req[have] = 0;
    char* eol = strchr(req, '\n');
    if (!eol) eol = req + have;
    if (eol > req && eol[-1] == '\r') eol--;
    *eol = 0;
    return (int)(eol - req);
}

int main(int argc, char** argv) {
    uint32_t port = PORT_DEFAULT;
    const char* dir = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-p")) {
            if (++i == argc || parse_uint(argv[i], &port) || port == 0 || port > 65535) return usage();
        } else if (argv[i][0] == '-' || dir) return usage();
        else dir = argv[i];
    }
    if (dir) root = dir;
    dirent_t d;
    int r = sys_stat(root, &d);
    if (r < 0 || !(d.flags & DIRENT_DIR)) {
        dprintf(STDERR_FILENO, "httpd: %s: %s\n", root, r < 0 ? strerror(r) : "not a directory");
        return 1;
    }

    int l = sys_socket(SOCK_STREAM);
    r = l;
    if (l >= 0) r = sys_bind(l, (int)port);
    if (r >= 0) r = sys_listen(l, 8);
    if (r < 0) {
        dprintf(STDERR_FILENO, "httpd: port %u: %s\n", port, strerror(r));
        return 1;
    }
    if (!quiet) dprintf(STDERR_FILENO, "Serving %s on port %u\n", root, port);

    for (;;) {
        sock_addr_t peer;
        int c = sys_accept(l, &peer, -1);
        if (c < 0) {
            if (c == -EINTR) break;
            dprintf(STDERR_FILENO, "httpd: accept: %s\n", strerror(c));
            continue;
        }
        outn = 0;
        out_bad = 0;
        int n = read_request(c);
        int status = 0;
        uint32_t sent = 0;
        char line[PATH_MAX + 16] = "-";
        if (n >= 0) {
            strlcpy(line, req, sizeof line);
            status = serve(c, req, &sent);
        } else if (n == -ETIMEDOUT) {
            fail(c, 408, 1);
            status = 408;
        } else if (n == -E2BIG) {
            fail(c, 400, 1);
            status = 400;
        }
        flush(c);
        sys_shutdown(c, SHUT_WR);
        sys_close(c);
        if (!quiet)
            dprintf(STDERR_FILENO, IPV4_FMT " \"%s\" %d%s\n", IPV4_ARGS(peer.ip), line, status, out_bad ? " (client gone)" : "");
        (void)sent;
    }
    sys_close(l);
    return 0;
}

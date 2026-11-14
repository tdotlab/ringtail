// printf subset shared by the kernel and user programs.
#include "lib.h"

typedef struct {
    char* buf;
    size_t size;
    size_t len;  // characters the full output needs, like C99 snprintf
} out_t;

static void put(out_t* o, char c) {
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

// Emit s (n chars) in a field of the given width. Zero padding goes
// after a leading sign, as in C.
static void field(out_t* o, const char* s, int n, int width, int left, char pad) {
    int fill = width > n ? width - n : 0;
    if (left) {
        for (int i = 0; i < n; i++) put(o, s[i]);
        while (fill--) put(o, ' ');
        return;
    }
    if (pad == '0' && n && s[0] == '-') { put(o, '-'); s++; n--; }
    while (fill--) put(o, pad);
    for (int i = 0; i < n; i++) put(o, s[i]);
}

// 64-bit values take the slow path only when they need it: dividing
// in 32 bits is the cheaper instruction.
static int fmt_num(char* out, uint64_t v, unsigned base, int upper, int neg) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0, k = 0;
    if (v <= UINT32_MAX) {
        uint32_t w = (uint32_t)v;
        do { tmp[n++] = digits[w % base]; w /= base; } while (w);
    } else {
        do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    }
    if (neg) out[k++] = '-';
    while (n) out[k++] = tmp[--n];
    return k;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    out_t o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        char pad = ' ';
        int left = 0, width = 0;
        for (;; fmt++) {
            if (*fmt == '0') pad = '0';
            else if (*fmt == '-') left = 1;
            else break;
        }
        if (*fmt == '*') {  // the width as an argument; negative means left
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        // Length: int unless l (long), ll (long long) or z (size_t).
        int lsize = sizeof(int);
        if (*fmt == 'l') { fmt++; lsize = sizeof(long); if (*fmt == 'l') { fmt++; lsize = sizeof(long long); } }
        else if (*fmt == 'z') { fmt++; lsize = sizeof(size_t); }
        char num[24];
        int n;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lsize == 8 ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            n = fmt_num(num, v < 0 ? 0u - (uint64_t)v : (uint64_t)v, 10, 0, v < 0);
            field(&o, num, n, width, left, pad);
            break;
        }
        case 'u': case 'x': case 'X': {
            uint64_t v = lsize == 8 ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            n = fmt_num(num, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', 0);
            field(&o, num, n, width, left, pad);
            break;
        }
        case 'p':
            put(&o, '0'); put(&o, 'x');
            n = fmt_num(num, (uintptr_t)va_arg(ap, void*), 16, 0, 0);
            field(&o, num, n, (int)sizeof(void*) * 2, 0, '0');
            break;
        case 'c': { char c = (char)va_arg(ap, int); field(&o, &c, 1, width, left, ' '); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            field(&o, s, (int)strlen(s), width, left, ' ');
            break;
        }
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

void sbprintf(sbuf_t* sb, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t room = sb->len < sb->size ? sb->size - sb->len : 0;
    sb->len += (size_t)vsnprintf(room ? sb->buf + sb->len : 0, room, fmt, ap);
    va_end(ap);
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

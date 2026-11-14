// Freestanding C library subset for the kernel.
#ifndef RINGTAIL_LIB_H
#define RINGTAIL_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strcasecmp(const char* a, const char* b);  // ASCII letters in either case match
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);  // the last c in s
// Copies at most size-1 bytes and always terminates. Returns strlen(src).
size_t strlcpy(char* dst, const char* src, size_t size);

// Text for an errno value (either sign).
const char* strerror(int err);

// Parses an unsigned decimal or 0x-prefixed hex number. Returns 0 on
// success, -1 on an empty string, a stray character or overflow.
int parse_uint(const char* s, uint32_t* out);
// A dotted IPv4 address, exactly four numbers 0-255 ("10.0.2.2"), in
// host order. 0, or -1 for anything else.
int parse_ipv4(const char* s, uint32_t* out);

// printf subset: %d %i %u %x %X %p %s %c %%, with the '0' and '-'
// flags, a field width (or *), and the length modifiers l, ll and z.
// An address prints as %lx of (unsigned long)addr, which is pointer
// sized; %p pads to the pointer's width.
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// Text built up a piece at a time in a fixed buffer. len counts what
// the whole text needs, so a caller can tell it was cut short.
typedef struct { char* buf; size_t size, len; } sbuf_t;
void sbprintf(sbuf_t* sb, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
// The mask takes x's type, so a 32-bit alignment cannot clear the
// high half of a 64-bit address.
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))

#endif

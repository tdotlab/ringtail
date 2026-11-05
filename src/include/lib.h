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

// printf subset: %d %i %u %x %X %p %s %c %%, with optional '0' flag,
// field width and the 'l' length modifier (ignored, int is 32 bits).
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// Text built up a piece at a time in a fixed buffer. len counts what
// the whole text needs, so a caller can tell it was cut short.
typedef struct { char* buf; size_t size, len; } sbuf_t;
void sbprintf(sbuf_t* sb, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#endif

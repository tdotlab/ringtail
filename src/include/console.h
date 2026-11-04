// Text console: VGA text mode plus a mirror on the serial port.
#ifndef RINGTAIL_CONSOLE_H
#define RINGTAIL_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

#define COLOR_DEFAULT 0x0F
#define COLOR_PROMPT  0x0A
#define COLOR_TITLE   0x0B
#define COLOR_DIM     0x08
#define COLOR_ERROR   0x0C
#define COLOR_OK      0x0A

void console_init(void);
void console_clear(void);
void console_set_color(uint8_t color);
uint8_t console_get_color(void);

// Output. Every call is atomic with respect to other tasks: one call's
// text is never split by another task's output. These may block, so
// they must not be used from interrupt handlers (use panic there).
void console_write(const char* s, size_t len);
void console_puts(const char* s);
void console_putc(char c);
void console_puts_color(const char* s, uint8_t color);
int kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int kprintf_color(uint8_t color, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Hold the console across several writes so they come out together.
void console_lock(void);
void console_unlock(void);

// Erase the character before the cursor (line editing).
void console_backspace(void);

// While graphics mode owns the screen, text goes to an off-screen copy
// and reappears when text mode comes back.
void console_detach(void);
void console_attach(void);

// Panic mode: bypass the lock so a dying kernel can always print.
void console_panic_mode(void);

#endif

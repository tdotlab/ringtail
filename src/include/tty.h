// Terminal input: keys from the PS/2 keyboard and bytes from the
// serial port arrive here as characters, in one queue.
#ifndef RINGTAIL_TTY_H
#define RINGTAIL_TTY_H

#include <stdint.h>

#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_L 0x0C
#define KEY_CTRL_W 0x17  // what Ctrl+Backspace sends: delete a word

// Navigation keys travel through the queue as single bytes from 0x80
// up: the key in bits 2-6, the Shift and Ctrl modifiers in bits 0-1.
// Programs in raw mode see them as the usual xterm escape sequences.
enum { NAV_UP, NAV_DOWN, NAV_RIGHT, NAV_LEFT, NAV_HOME, NAV_END,
       NAV_INSERT, NAV_DELETE, NAV_PGUP, NAV_PGDN, NAV_COUNT };
#define KEY_MOD_SHIFT 1
#define KEY_MOD_CTRL  2
#define KEY_NAV(k, mods) (0x80 | ((k) << 2) | (mods))
#define KEY_IS_NAV(c)    ((c) >= 0x80 && (c) < KEY_NAV(NAV_COUNT, 0))
#define KEY_NAV_KEY(c)   (((c) >> 2) & 0x1F)
#define KEY_NAV_MODS(c)  ((c) & 3)
#define KEY_UP    KEY_NAV(NAV_UP, 0)
#define KEY_DOWN  KEY_NAV(NAV_DOWN, 0)
#define KEY_LEFT  KEY_NAV(NAV_LEFT, 0)
#define KEY_RIGHT KEY_NAV(NAV_RIGHT, 0)

// Called from interrupt handlers.
void tty_input(int c);

// Blocking read of one character. Returns -EINTR if the calling
// task is killed while waiting.
int tty_getc(void);
int tty_pending(void);

// Read for programs. Cooked mode: a line at a time with echo and
// backspace, Ctrl+D at the start of a line for end of file; the line
// keeps its '\n'. Raw mode (TTY_RAW): whatever bytes are available,
// at least one, no echo. Returns bytes stored, 0 at end of file, or
// -errno.
int tty_read(char* buf, uint32_t len);

// Mode flags (TTY_RAW, TTY_NOSIG in ringtail/abi.h). Only the
// foreground job may change them, and they go back to cooked
// whenever the foreground changes.
int tty_set_mode(int mode);

// The foreground job receives input and Ctrl+C. 0 means the shell.
void tty_set_foreground(int job);
int tty_foreground(void);

// Channel woken on any input (keyboard, serial or mouse).
extern int input_chan;

#endif

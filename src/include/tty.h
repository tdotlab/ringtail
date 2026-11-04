// Terminal input: keys from the PS/2 keyboard and bytes from the
// serial port arrive here as characters, in one queue.
#ifndef RINGTAIL_TTY_H
#define RINGTAIL_TTY_H

#include <stdint.h>

#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_L 0x0C
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83

// Called from interrupt handlers.
void tty_input(int c);

// Blocking read of one character. Returns -EINTR if the calling
// task is killed while waiting.
int tty_getc(void);
int tty_pending(void);

// Cooked line read for programs: echo, backspace, Enter, Ctrl+D at
// the start of a line for end of file. Returns bytes stored (the
// line keeps its '\n'), 0 at end of file, or -errno.
int tty_read(char* buf, uint32_t len);

// The foreground job receives input and Ctrl+C. 0 means the shell.
void tty_set_foreground(int job);
int tty_foreground(void);

// Channel woken on any input (keyboard, serial or mouse).
extern int input_chan;

#endif

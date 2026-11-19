// Terminals: the key codes that travel through a terminal's input
// queue (shared with the programs), and the kernel's terminals, the
// console and ptys (tty.c).
#ifndef RINGTAIL_TTY_H
#define RINGTAIL_TTY_H

#include <stdint.h>

#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_L 0x0C
#define KEY_CTRL_BACKSLASH 0x1C  // the kill switch, even for raw programs
#define KEY_CTRL_W 0x17  // what Ctrl+Backspace sends: delete a word

// Navigation and function keys travel through the queue as single
// bytes from 0x80 up: the key in bits 2-6, the Shift and Ctrl
// modifiers in bits 0-1. Programs in raw mode see them as the usual
// xterm escape sequences. Alt+key arrives as ESC followed by the key,
// as xterm sends it.
enum { NAV_UP, NAV_DOWN, NAV_RIGHT, NAV_LEFT, NAV_HOME, NAV_END,
       NAV_INSERT, NAV_DELETE, NAV_PGUP, NAV_PGDN,
       NAV_F1, NAV_F12 = NAV_F1 + 11, NAV_COUNT };
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

// The kernel's side. A terminal is the console or a pty (tty.c says
// how they work); console_tty is the console, and tty_current the
// calling task's controlling terminal.
struct tty;
extern struct tty* const console_tty;
struct tty* tty_current(void);
int tty_is_console(const struct tty* t);

// A key for the console, from the keyboard and serial interrupt
// handlers.
void tty_input(int c);

// Input waiting: a key, the rest of a line, or a hangup (so a read
// would not block).
int tty_pending(struct tty* t);
// The same, and the calling task's job has the terminal.
int tty_has_input(struct tty* t);

// Read for programs. Cooked mode: a line at a time with echo and
// backspace, Ctrl+D at the start of a line for end of file; the line
// keeps its '\n'. Raw mode (TTY_RAW): whatever bytes are available,
// at least one, no echo. Returns bytes stored, 0 at end of file (or
// once a pty has hung up), or -errno.
int tty_read(struct tty* t, char* buf, uint32_t len);

// Output. tty_write may block (a pty whose master is slow to read) and
// fails with EIO once a pty has hung up. Hold the output lock to keep
// several writes together.
int tty_write(struct tty* t, const char* s, uint32_t len);
void tty_lock_output(struct tty* t);
void tty_unlock_output(struct tty* t);
int tty_out_events(struct tty* t);  // POLL* for a writer
void tty_set_color(struct tty* t, uint8_t color);  // SYS_SET_COLOR
int tty_size(struct tty* t, int* cols, int* rows);

// Mode flags (TTY_RAW, TTY_NOSIG in ringtail/abi.h). Only the
// foreground job may change them, and they go back to cooked
// whenever the foreground changes.
int tty_set_mode(struct tty* t, int mode);

// The foreground job receives input, Ctrl+C and Ctrl+backslash.
// tty_set_foreground is for the kernel (init, and a pty's first job);
// it forgets any handovers. tty_handover is SYS_TTY_FG, with its rules
// (abi.h). A pty is unowned while no live job has it: then a spawn
// with SPAWN_FOREGROUND may give it to the new job.
void tty_set_foreground(struct tty* t, int job);
int tty_handover(struct tty* t, int job);
int tty_foreground(struct tty* t);
int tty_unowned(struct tty* t);

// Ptys. pty_new makes one with a reference for its master; the master
// side's file calls the pty_master_ functions and, as it closes,
// pty_master_close (which drops that reference). Terminal descriptors
// count themselves with tty_slave_ref, tasks with tty_ref; all of
// these do nothing for the console.
struct tty* pty_new(int cols, int rows);
void tty_ref(struct tty* t);
void tty_unref(struct tty* t);
void tty_slave_ref(struct tty* t);
void tty_slave_unref(struct tty* t);
int pty_master_read(struct tty* t, uint8_t* buf, uint32_t len);
int pty_master_write(struct tty* t, const uint8_t* buf, uint32_t len);
int pty_master_events(struct tty* t);
void pty_master_close(struct tty* t);

// Channel woken on any input (keyboard, serial, mouse or a pty).
extern int input_chan;

#endif

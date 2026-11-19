// Scancode set 1 to keys, for a US layout: the keyboard driver turns
// keys into terminal input with it, and the window server into key
// events, so a window gets the codes the terminal would. Shared by the
// kernel and the programs.
#ifndef RINGTAIL_KEYMAP_H
#define RINGTAIL_KEYMAP_H

#include <stdint.h>
#include "tty.h"  // KEY_NAV and the NAV_ keys

#define KEY_MOD_ALT 4  // with tty.h's KEY_MOD_SHIFT and KEY_MOD_CTRL

// The modifiers held, Caps Lock, and whether an E0 prefix came last.
// Zero it to start.
typedef struct {
    uint8_t ext, lshift, rshift, lctrl, rctrl, lalt, ralt, caps;
} keymap_t;

typedef struct {
    int key;   // the scancode without its release bit, plus 0x100 after E0
    int down;  // 1 pressed or repeating, 0 released
    int mods;  // KEY_MOD_* held, this key included if it is one
    // What the terminal gets for it: a character (Ctrl+letter as a
    // control character), a KEY_NAV code, or 0 for nothing (releases,
    // modifiers, Caps Lock, keys without a character).
    int code;
    int esc;   // the terminal gets ESC before code: Alt with a character
} keyev_t;

// Feed one byte from the keyboard. 1 when it finished a key event in
// *ev, 0 when it was a prefix or a fake shift some keyboards send
// around the navigation keys.
int keymap_feed(keymap_t* k, uint8_t sc, keyev_t* ev);

#endif

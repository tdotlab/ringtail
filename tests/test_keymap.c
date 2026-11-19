// Host test for the scancode translation (src/lib/keymap.c) that the
// keyboard driver and the window server share.
#include <stdio.h>
#include <string.h>
#include "keymap.h"

static int fails, checks;

static void check(int ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static keymap_t km;

// Feed bytes; the last event made is in *ev, and the count of events.
static int feed(keyev_t* ev, int n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    int made = 0;
    for (int i = 0; i < n; i++) {
        keyev_t e;
        if (keymap_feed(&km, (uint8_t)__builtin_va_arg(ap, int), &e)) {
            *ev = e;
            made++;
        }
    }
    __builtin_va_end(ap);
    return made;
}

// The terminal code for one press and release of scancode sc.
static int tap(int sc) {
    keyev_t e, r;
    feed(&e, 1, sc);
    feed(&r, 1, sc | 0x80);
    return r.code ? -1 : e.code;  // a release never has a code
}

int main(void) {
    keyev_t e;
    check(feed(&e, 1, 0x1E) == 1 && e.key == 0x1E && e.down && e.code == 'a' && e.mods == 0 && !e.esc, "a");
    check(feed(&e, 1, 0x9E) == 1 && e.key == 0x1E && !e.down && e.code == 0, "its release has the key, no code");
    check(tap(0x02) == '1' && tap(0x1C) == '\n' && tap(0x0E) == '\b' && tap(0x39) == ' ' && tap(0x01) == 0x1B,
          "digits, Enter, Backspace, space, Esc");

    feed(&e, 1, 0x2A);
    check(e.key == 0x2A && e.down && e.code == 0 && e.mods == KEY_MOD_SHIFT, "Shift down is an event with no code");
    check(tap(0x1E) == 'A' && tap(0x02) == '!' && tap(0x2B) == '|', "shifted keys");
    feed(&e, 1, 0xAA);
    check(e.mods == 0 && tap(0x1E) == 'a', "Shift up");
    feed(&e, 2, 0x36, 0x2A);
    feed(&e, 1, 0xB6);
    check(tap(0x1E) == 'A', "one shift still held");
    feed(&e, 1, 0xAA);

    feed(&e, 2, 0x3A, 0xBA);
    check(tap(0x1E) == 'A' && tap(0x02) == '1', "Caps Lock: letters only");
    feed(&e, 1, 0x2A);
    check(tap(0x1E) == 'a' && tap(0x02) == '!', "Caps Lock and Shift");
    feed(&e, 3, 0xAA, 0x3A, 0xBA);
    check(tap(0x1E) == 'a', "Caps Lock off");

    feed(&e, 1, 0x1D);
    check(e.mods == KEY_MOD_CTRL, "Ctrl down");
    check(tap(0x2E) == 0x03 && tap(0x2B) == KEY_CTRL_BACKSLASH && tap(0x0E) == KEY_CTRL_W,
          "Ctrl+C, Ctrl+backslash, Ctrl+Backspace");
    check(tap(0x02) == 0, "Ctrl+digit gives nothing");
    feed(&e, 1, 0x9D);
    feed(&e, 2, 0xE0, 0x1D);
    check(e.key == 0x11D && e.mods == KEY_MOD_CTRL && tap(0x2E) == 0x03, "right Ctrl");
    feed(&e, 2, 0xE0, 0x9D);
    check(e.mods == 0, "right Ctrl up");

    feed(&e, 1, 0x38);
    feed(&e, 1, 0x2D);
    check(e.code == 'x' && e.esc && e.mods == KEY_MOD_ALT, "Alt+x: ESC then x");
    feed(&e, 1, 0x48);
    check(e.code == KEY_UP && !e.esc, "Alt does not prefix the arrows");
    feed(&e, 2, 0xC8, 0xB8);
    check(e.mods == 0, "Alt up");

    check(feed(&e, 2, 0xE0, 0x48) == 1 && e.key == 0x148 && e.code == KEY_UP, "Up, with E0");
    check(tap(0x48) == KEY_UP && tap(0x4B) == KEY_LEFT, "and from the keypad without it");
    feed(&e, 1, 0x2A);
    feed(&e, 2, 0xE0, 0x4D);
    check(e.code == KEY_NAV(NAV_RIGHT, KEY_MOD_SHIFT), "Shift+Right carries the modifier");
    feed(&e, 2, 0xE0, 0xCD);
    feed(&e, 1, 0xAA);
    check(tap(0x3B) == KEY_NAV(NAV_F1, 0) && tap(0x44) == KEY_NAV(NAV_F1 + 9, 0) && tap(0x58) == KEY_NAV(NAV_F12, 0),
          "F1, F10, F12");
    check(feed(&e, 2, 0xE0, 0x47) == 1 && e.code == KEY_NAV(NAV_HOME, 0), "Home");
    feed(&e, 2, 0xE0, 0xC7);
    check(tap(0x47) == 0, "keypad 7 without E0 is not Home");
    check(feed(&e, 2, 0xE0, 0x1C) == 1 && e.code == '\n' && e.key == 0x11C, "keypad Enter");
    feed(&e, 2, 0xE0, 0x9C);
    check(feed(&e, 2, 0xE0, 0x35) == 1 && e.code == '/', "keypad slash");
    feed(&e, 2, 0xE0, 0xB5);
    check(feed(&e, 2, 0xE0, 0x5B) == 1 && e.key == 0x15B && e.code == 0, "the Windows key: an event, no code");
    feed(&e, 2, 0xE0, 0xDB);

    check(feed(&e, 2, 0xE0, 0x2A) == 0 && feed(&e, 2, 0xE0, 0xAA) == 0, "E0 2A and E0 AA are not events");
    check(tap(0x1E) == 'a', "and do not hold Shift");
    feed(&e, 1, 0x2A);
    feed(&e, 2, 0xE0, 0xAA);
    check(tap(0x1E) == 'A', "nor let it go");
    feed(&e, 1, 0xAA);
    check(feed(&e, 1, 0xE0) == 0 && feed(&e, 1, 0x1E) == 1 && e.key == 0x11E, "a prefix applies to the next byte");
    check(feed(&e, 1, 0x1E) == 1 && e.key == 0x1E, "and only that one");
    check(feed(&e, 1, 0x7F) == 1 && e.code == 0 && feed(&e, 1, 0xFF) == 1 && !e.down, "bytes past the map give no code");

    printf("test_keymap: %d of %d checks %s\n", checks - fails, checks, fails ? "passed, FAILURES above" : "passed");
    return fails != 0;
}

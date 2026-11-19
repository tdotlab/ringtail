// Scancode set 1 to keys (keymap.h).
#include "keymap.h"

static const char normal_map[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};
static const char shift_map[128] = {
    0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

#define SC_LCTRL     0x1D
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CAPS      0x3A
#define SC_BACKSPACE 0x0E
#define SC_ALT       0x38

// Navigation keys. The arrows answer with or without the E0 prefix
// (the keypad sends them without, while Num Lock is off, which is
// all this knows). The rest need E0, so the keypad keeps its digits
// there.
static int nav_key(uint8_t code, int ext) {
    switch (code) {
    case 0x48: return NAV_UP;
    case 0x50: return NAV_DOWN;
    case 0x4B: return NAV_LEFT;
    case 0x4D: return NAV_RIGHT;
    }
    if (!ext) {
        if (code >= 0x3B && code <= 0x44) return NAV_F1 + (code - 0x3B);  // F1-F10
        if (code == 0x57) return NAV_F1 + 10;
        if (code == 0x58) return NAV_F1 + 11;
        return -1;
    }
    switch (code) {
    case 0x47: return NAV_HOME;
    case 0x4F: return NAV_END;
    case 0x52: return NAV_INSERT;
    case 0x53: return NAV_DELETE;
    case 0x49: return NAV_PGUP;
    case 0x51: return NAV_PGDN;
    }
    return -1;
}

// The character for a key pressed with no E0 before it, or 0.
static int char_code(const keymap_t* k, uint8_t code, int shift, int ctrl) {
    if (ctrl && code == SC_BACKSPACE) return KEY_CTRL_W;
    char c = shift ? shift_map[code] : normal_map[code];
    if (!c) return 0;
    if (c >= 'a' && c <= 'z' && k->caps && !shift) c = (char)(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z' && k->caps && shift) c = (char)(c - 'A' + 'a');
    if (ctrl) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return c & 0x1F;
        if (c == '\\' || c == '|') return KEY_CTRL_BACKSLASH;
        return 0;
    }
    return (uint8_t)c;
}

int keymap_feed(keymap_t* k, uint8_t sc, keyev_t* ev) {
    if (sc == 0xE0) { k->ext = 1; return 0; }
    int ext = k->ext;
    k->ext = 0;
    int down = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    if (code == SC_LCTRL) {
        if (ext) k->rctrl = (uint8_t)down; else k->lctrl = (uint8_t)down;
    } else if (code == SC_ALT) {
        if (ext) k->ralt = (uint8_t)down; else k->lalt = (uint8_t)down;
    } else if (code == SC_LSHIFT || code == SC_RSHIFT) {
        if (ext) return 0;  // E0 2A and E0 AA are not the user's shift
        if (code == SC_LSHIFT) k->lshift = (uint8_t)down; else k->rshift = (uint8_t)down;
    }
    int shift = k->lshift || k->rshift, ctrl = k->lctrl || k->rctrl, alt = k->lalt || k->ralt;
    ev->key = code | (ext ? 0x100 : 0);
    ev->down = down;
    ev->mods = (shift ? KEY_MOD_SHIFT : 0) | (ctrl ? KEY_MOD_CTRL : 0) | (alt ? KEY_MOD_ALT : 0);
    ev->code = 0;
    ev->esc = 0;
    if (!down || code == SC_LCTRL || code == SC_ALT || code == SC_LSHIFT || code == SC_RSHIFT) return 1;
    if (code == SC_CAPS) {
        k->caps = !k->caps;
        return 1;
    }
    int nav = nav_key(code, ext);
    if (nav >= 0) ev->code = KEY_NAV(nav, ev->mods & (KEY_MOD_SHIFT | KEY_MOD_CTRL));
    else if (ext) ev->code = code == 0x1C ? '\n' : code == 0x35 ? '/' : 0;  // the keypad's Enter and slash
    else {
        ev->code = char_code(k, code, shift, ctrl);
        ev->esc = alt && ev->code;  // Alt+key: ESC, then the key, as xterm sends it
    }
    return 1;
}

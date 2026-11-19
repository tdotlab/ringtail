// The window system's protocol: what pleroma, the window server, and
// its clients say to each other over a channel (SYS_CHAN_CONNECT to
// WS_CHANNEL). One message is one ws_msg_t. A connection has at most
// one window.
//
// A client makes shared memory for its pixels (SYS_SHM_NEW, w*h*4
// bytes, 0xRRGGBB, rows packed), says HELLO, then CREATE with the
// memory's id, and waits for SHOWN. After that it draws into the memory
// whenever it likes and sends DAMAGE for what it changed; the server
// copies from the memory when it draws the screen. Closing the channel
// (or exiting) takes the window away.
//
// The server never waits on a client: when a client's queue is full,
// events for it are dropped (a client that reads its events never sees
// that happen). When the server goes, every client reads end of file.
#ifndef RINGTAIL_WSYS_H
#define RINGTAIL_WSYS_H

#include <stdint.h>

#define WS_CHANNEL   "pleroma"
#define WS_TITLE_MAX 40  // bytes of a title, with its 0

enum {
    // Client to server.
    WS_HELLO = 1,  // hello.pid: the client's task id, so the server can
                   // end it if it does not close when asked (only if
                   // the server started it)
    WS_CREATE,     // create: a window of w x h pixels, in shared memory shm
    WS_DAMAGE,     // damage: the pixels in x, y, w, h changed
    WS_TITLE,      // title: a new title
    // Server to client.
    WS_SHOWN = 16, // shown.error: 0, the window is up; or why not
                   // (-EINVAL: bad size or memory, -ENOMEM: no room,
                   // -EEXIST: this connection has a window already)
    WS_MOUSE,      // mouse: the pointer at x, y in the window (outside
                   // it while a button held since a press inside), and
                   // the buttons (1 left, 2 right, 4 middle)
    WS_KEY,        // key: a key for the focused window (keymap.h's keyev_t)
    WS_FOCUS,      // focus.on: the window got (1) or lost (0) the keys
    WS_CLOSE,      // the user asked the window to close
};

typedef struct {
    uint32_t type;
    union {
        struct { int32_t pid; } hello;
        struct { uint32_t shm; int32_t w, h; char title[WS_TITLE_MAX]; } create;
        struct { int32_t x, y, w, h; } damage;
        struct { char title[WS_TITLE_MAX]; } title;
        struct { int32_t error; } shown;
        struct { int32_t x, y, buttons; } mouse;
        // key: the scancode (plus 0x100 after E0), 1 down or 0 up, the
        // KEY_MOD_ bits, and what a terminal would get (a character, a
        // KEY_NAV code, or 0); Alt shows in mods.
        struct { int32_t key, down, mods, code; } key;
        struct { int32_t on; } focus;
    };
} ws_msg_t;

#endif

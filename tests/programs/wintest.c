// The window system from a client's side, run by pleroma ("pleroma
// wintest"): what the server refuses, on a raw connection, then a
// window of a colour nothing else on the desktop has. Its checks are
// summed up as soon as the window is up; after that it prints each
// event it gets, a line each, for qemu_test.py to follow, and answers
// some keys: F fills the window with a second colour, S makes it ignore
// requests to close (so pleroma has to end it). "wintest none" checks
// what a client gets with no server running.
#include "ulib.h"
#include "win.h"

#define WIN_W   120
#define WIN_H   80
#define COLOUR  0x00A050
#define COLOUR2 0x5000A0

// CREATE on a raw connection; what SHOWN says, or 1 if nothing came.
static int raw_create(int fd, uint32_t shm, int w, int h) {
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_CREATE;
    m.create.shm = shm;
    m.create.w = w;
    m.create.h = h;
    strlcpy(m.create.title, "raw", sizeof m.create.title);
    sys_write(fd, &m, sizeof m);
    // Events (FOCUS, once a window is up) can come first; pass over them.
    for (;;) {
        pollfd_t p = { fd, POLLIN, 0 };
        if (sys_pollfd(&p, 1, 5000) != 1) return 1;
        memset(&m, 0, sizeof m);
        if (sys_read(fd, &m, sizeof m) != (int)sizeof m) return 1;
        if (m.type == WS_SHOWN) return m.shown.error;
    }
}

int main(int argc, char** argv) {
    win_t w;
    if (argc > 1 && !strcmp(argv[1], "none")) {
        check(win_open(&w, 10, 10, "x") == -ECONNREFUSED, "with no server, a window is refused");
        return check_summary("wintest");
    }

    int c = sys_chan_connect(WS_CHANNEL);
    check(c >= 0, "connect to the window server");
    shm_info_t small, wide;
    int id = sys_shm_new(4096, &small), id2 = sys_shm_new(8 * 4096, &wide);
    check(raw_create(c, 0, 10, 10) == -EINVAL, "memory with an unknown id is refused");
    check(raw_create(c, (uint32_t)id, 64, 64) == -EINVAL, "so is memory too small for the window");
    check(raw_create(c, (uint32_t)id, 0, 10) == -EINVAL && raw_create(c, (uint32_t)id, 10, -1) == -EINVAL,
          "and an empty window");
    check(raw_create(c, (uint32_t)id2, 8000, 1) == -ENOMEM, "a window wider than the screen has no room");
    check(raw_create(c, (uint32_t)id, 16, 16) == 0, "a window that fits is shown");
    check(raw_create(c, (uint32_t)id, 16, 16) == -EEXIST, "a connection has one window");
    ws_msg_t junk;
    memset(&junk, 0, sizeof junk);
    junk.type = 99;
    sys_write(c, "xy", 2);
    sys_write(c, &junk, sizeof junk);
    check(raw_create(c, (uint32_t)id, 16, 16) == -EEXIST, "short and unknown messages are passed over");
    sys_close(c);
    sys_shm_unmap((uintptr_t)small.addr);
    sys_shm_unmap((uintptr_t)wide.addr);

    check(win_open(&w, WIN_W, WIN_H, "wintest") == 0 && w.pixels, "open a window");
    win_fill(&w, 0, 0, WIN_W, WIN_H, COLOUR);
    win_damage(&w, 0, 0, WIN_W, WIN_H);
    if (check_summary("wintest")) return 1;
    printf("wintest: up\n");

    int stubborn = 0;
    ws_msg_t ev;
    for (;;) {
        int e = win_event(&w, &ev, -1);
        if (e <= 0) {
            printf("wintest: server gone\n");
            break;
        }
        if (ev.type == WS_MOUSE) {
            printf("wintest: mouse %d %d %d\n", ev.mouse.x, ev.mouse.y, ev.mouse.buttons);
        } else if (ev.type == WS_KEY) {
            printf("wintest: key %x %d %d %d\n", ev.key.key, ev.key.down, ev.key.mods, ev.key.code);
            if (ev.key.down && (ev.key.code == 'f' || ev.key.code == 'F')) {
                win_fill(&w, 0, 0, WIN_W, WIN_H, COLOUR2);
                win_damage(&w, 0, 0, WIN_W, WIN_H);
            }
            if (ev.key.down && (ev.key.code == 's' || ev.key.code == 'S')) stubborn = 1;
        } else if (ev.type == WS_FOCUS) {
            printf("wintest: focus %d\n", ev.focus.on);
        } else if (ev.type == WS_CLOSE) {
            printf(stubborn ? "wintest: close ignored\n" : "wintest: close\n");
            if (!stubborn) break;
        } else {
            printf("wintest: event %u\n", ev.type);
        }
    }
    win_close(&w);
    return 0;
}

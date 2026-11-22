// Pleroma, the window server: takes the display, listens on the
// WS_CHANNEL channel (wsys.h), and draws the windows its clients make
// on a desktop with a bar. The programs named on its command line are
// started with it, or with none named, kenoma, the launcher; the
// Ringtail button, or a right click on the desktop, opens a menu that
// starts more or leaves. So does Ctrl+Alt+Backspace (leaving). The
// desktop's colours come from desk.h's file, and a client can change
// them (the settings window does).
//
// Everything waits in one place, a poll of the keyboard and mouse, the
// listener and every client. Each time round, what changed is drawn
// into the frame (wm.c composes it), the pointer goes on top, and just
// those rectangles go to the screen. A client's pixels are read
// straight out of its shared memory when its window is drawn.
//
// The mouse: a press focuses and raises a window; the title bar drags
// it; the close button asks its program to close, and a second press
// at least two seconds later ends the program, if pleroma started it.
// A press in a window's content goes to it, and so does every move
// until the buttons are all up, wherever the pointer goes.
#include "ulib.h"
#include "keymap.h"
#include "wm.h"
#include "wsys.h"
#include "desk.h"

#define MAX_CLIENTS WM_MAX  // with stdin, stdout, stderr and the listener: MAX_FDS
#define PTR_SHADOW 2
#define PTR_W (11 + PTR_SHADOW)  // the pointer and its shadow
#define PTR_H (17 + PTR_SHADOW)
#define SC_BACKSPACE 0x0E
#define SC_ESC 0x01
#define CLOSE_WAIT 200      // ticks before a second close ends a program

typedef struct {
    int fd;            // the channel; -1 for a free slot
    int pid;           // from HELLO, or 0
    int win;           // its window, or -1
    uint32_t* pixels;  // the window's memory, mapped here
    int closing;       // asked to close, at close_at
    uint32_t close_at;
} client_t;

static client_t clients[MAX_CLIENTS];
static wm_t wm;
static int W, H;
static uint32_t* frame;
static uint8_t font[256 * 16];
static int listener;
static mouse_state_t mouse;
static int drag = -1, drag_dx, drag_dy;  // the window being dragged
static int grab = -1;                    // the client getting the mouse until the buttons are up
static int focused = -1;                 // the client last told it has the focus
static int quit;
static int started[32], nstarted;        // programs started here, which may be ended

static const char* const menu_items[] = { "Programs", "Files", "Terminal", "Clock", "Paint", "Settings", "Leave" };
static const char* const menu_programs[] = { "KENOMA", "WFILES", "WTERM", "WCLOCK", "WPAINT", "WPREFS", 0 };
#define MENU_N 7

// ---- Clients ----

// Only if it can be sent without waiting: a client that does not read
// misses events rather than stopping the desktop.
static void send(int c, ws_msg_t* m) {
    pollfd_t p = { clients[c].fd, POLLOUT, 0 };
    if (sys_pollfd(&p, 1, 0) == 1 && (p.revents & POLLOUT) && !(p.revents & POLLHUP))
        sys_write(clients[c].fd, m, sizeof *m);
}

static void send_simple(int c, uint32_t type, int32_t v) {
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.shown.error = v;  // shown.error and focus.on share the place
    send(c, &m);
}

static void drop(int c) {
    client_t* k = &clients[c];
    if (k->win >= 0) {
        if (drag == k->win) drag = -1;  // the slot may go to a new window
        wm_remove(&wm, k->win);
    }
    if (k->pixels) sys_shm_unmap((uintptr_t)k->pixels);
    sys_close(k->fd);
    memset(k, 0, sizeof *k);
    k->fd = -1;
    k->win = -1;
    if (grab == c) grab = -1;
    if (focused == c) focused = -1;
}

static int client_of(int win) { return win >= 0 ? wm.win[win].tag : -1; }

// Tells the clients that lost and got the focus.
static void focus_changed(void) {
    int now = wm.focus >= 0 ? client_of(wm.focus) : -1;
    if (now == focused) return;
    if (focused >= 0 && clients[focused].fd >= 0) send_simple(focused, WS_FOCUS, 0);
    if (now >= 0) send_simple(now, WS_FOCUS, 1);
    focused = now;
}

static void create(int c, ws_msg_t* m) {
    client_t* k = &clients[c];
    if (k->win >= 0) {
        send_simple(c, WS_SHOWN, -EEXIST);
        return;
    }
    shm_info_t in;
    int w = m->create.w, h = m->create.h;
    if (sys_shm_map(m->create.shm, &in) < 0) {
        send_simple(c, WS_SHOWN, -EINVAL);
        return;
    }
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h * 4 > in.size) {
        sys_shm_unmap((uintptr_t)in.addr);
        send_simple(c, WS_SHOWN, -EINVAL);
        return;
    }
    m->create.title[WS_TITLE_MAX - 1] = 0;
    int win = wm_add(&wm, w, h, in.addr, m->create.title, c);
    if (win < 0) {
        sys_shm_unmap((uintptr_t)in.addr);
        send_simple(c, WS_SHOWN, -ENOMEM);
        return;
    }
    k->win = win;
    k->pixels = in.addr;
    send_simple(c, WS_SHOWN, 0);
}

static void message(int c, ws_msg_t* m, int n) {
    client_t* k = &clients[c];
    if (n < (int)sizeof m->type) return;
    switch (m->type) {
    case WS_HELLO:
        k->pid = m->hello.pid;
        break;
    case WS_CREATE:
        create(c, m);
        break;
    case WS_DAMAGE:
        if (k->win >= 0) wm_damage_content(&wm, k->win, m->damage.x, m->damage.y, m->damage.w, m->damage.h);
        break;
    case WS_TITLE:
        m->title.title[WS_TITLE_MAX - 1] = 0;
        if (k->win >= 0) wm_set_title(&wm, k->win, m->title.title);
        break;
    case WS_DESKTOP:
        wm_set_desktop(&wm, m->desktop.top, m->desktop.bottom);
        break;
    }
}

// Everything the client has sent, up to a queue's worth; 0 when it has
// gone.
static int read_client(int c) {
    for (int i = 0; i < CHAN_QUEUE; i++) {
        pollfd_t p = { clients[c].fd, POLLIN, 0 };
        if (sys_pollfd(&p, 1, 0) != 1 || !(p.revents & POLLIN)) return 1;
        ws_msg_t m;
        memset(&m, 0, sizeof m);
        int n = sys_read(clients[c].fd, &m, sizeof m);
        if (n <= 0) return 0;
        message(c, &m, n);
    }
    return 1;
}

static void accept_client(void) {
    int fd = sys_accept(listener, 0, 0);
    if (fd < 0) return;
    for (int c = 0; c < MAX_CLIENTS; c++)
        if (clients[c].fd < 0) {
            clients[c].fd = fd;
            return;
        }
    sys_close(fd);  // full: it reads the end
}

// ---- Starting and ending programs ----

static void start(const char* name) {
    const char* argv[] = { name, 0 };
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { -1, -1, -1 }, -1 };
    int id = sys_spawn_opts(name, argv, &o);
    if (id > 0) started[nstarted++ % 32] = id;
}

static int started_here(int pid) {
    for (int i = 0; i < 32 && i < nstarted; i++)
        if (started[i] == pid) return 1;
    return 0;
}

static void ask_close(int c) {
    client_t* k = &clients[c];
    uint32_t now = sys_ticks();
    if (k->closing && now - k->close_at >= CLOSE_WAIT && k->pid > 0 && started_here(k->pid)) {
        sys_kill(k->pid, SIGKILL);  // its window goes when its channel does
        return;
    }
    if (!k->closing) k->close_at = now;
    k->closing = 1;
    send_simple(c, WS_CLOSE, 0);
}

static void pick(int item) {
    if (item < 0 || item >= MENU_N) return;
    if (menu_programs[item]) start(menu_programs[item]);
    else quit = 1;
}

// ---- Input ----

static void send_mouse(int c, const mouse_state_t* m) {
    if (c < 0 || clients[c].win < 0) return;
    const wm_win_t* w = &wm.win[clients[c].win];
    ws_msg_t e;
    memset(&e, 0, sizeof e);
    e.type = WS_MOUSE;
    e.mouse.x = m->x - w->x;
    e.mouse.y = m->y - w->y;
    e.mouse.buttons = m->buttons;
    send(c, &e);
}

static void mouse_changed(const mouse_state_t* m) {
    mouse_state_t o = mouse;
    mouse = *m;
    int moved = m->x != o.x || m->y != o.y;
    if (moved) {
        wm_damage(&wm, o.x, o.y, PTR_W, PTR_H);
        wm_damage(&wm, m->x, m->y, PTR_W, PTR_H);
    }
    int pressed = m->buttons & ~o.buttons;
    if (drag >= 0) {
        if (m->buttons & 1) wm_move(&wm, drag, m->x - drag_dx, m->y - drag_dy);
        else drag = -1;
        return;
    }
    if (grab >= 0) {
        int c = grab;
        if (!m->buttons) grab = -1;
        send_mouse(c, m);
        return;
    }
    wm_hit_t h = wm_hit(&wm, m->x, m->y);
    if (wm.menu_open) {
        wm_menu_hover(&wm, h.part == WM_MENU_ITEM ? h.item : -1);
        if (pressed) {
            wm_menu_close(&wm);
            if (h.part == WM_MENU_ITEM) pick(h.item);
        }
        return;
    }
    if (!pressed) {
        if (moved && h.part == WM_CONTENT) send_mouse(client_of(h.win), m);
        return;
    }
    switch (h.part) {
    case WM_BAR_MENU:
        wm_menu_open(&wm, 4, WM_BAR_H, menu_items, MENU_N);
        break;
    case WM_DESKTOP:
    case WM_BAR:
        if (pressed & 2) wm_menu_open(&wm, m->x, m->y, menu_items, MENU_N);
        break;
    case WM_BAR_WIN:
    case WM_FRAME:
        wm_raise(&wm, h.win);
        break;
    case WM_TITLE:
        wm_raise(&wm, h.win);
        if (pressed & 1) {
            wm_rect_t f = wm_frame(&wm, h.win);
            drag = h.win;
            drag_dx = m->x - f.x;
            drag_dy = m->y - f.y;
        }
        break;
    case WM_CLOSE:
        wm_raise(&wm, h.win);
        if (pressed & 1) ask_close(client_of(h.win));
        break;
    case WM_CONTENT:
        wm_raise(&wm, h.win);
        grab = client_of(h.win);
        send_mouse(grab, m);
        break;
    }
}

static keymap_t keys;

static void key(const keyev_t* e) {
    int ctrl_alt = (e->mods & (KEY_MOD_CTRL | KEY_MOD_ALT)) == (KEY_MOD_CTRL | KEY_MOD_ALT);
    if (e->down && e->key == SC_BACKSPACE && ctrl_alt) {
        quit = 1;
        return;
    }
    if (wm.menu_open) {
        if (e->down && e->key == SC_ESC) wm_menu_close(&wm);
        return;
    }
    int c = client_of(wm.focus);
    if (c < 0) return;
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_KEY;
    m.key.key = e->key;
    m.key.down = e->down;
    m.key.mods = e->mods;
    m.key.code = e->code;
    send(c, &m);
}

static void input(void) {
    int sc;
    while ((sc = sys_key_get()) >= 0) {
        keyev_t e;
        if (keymap_feed(&keys, (uint8_t)sc, &e)) key(&e);
    }
    mouse_state_t m;
    sys_mouse_state(&m);
    if (m.x != mouse.x || m.y != mouse.y || m.buttons != mouse.buttons) mouse_changed(&m);
}

// ---- The screen ----

// The arrow: X its outline, . its fill. Its shadow is the same shape,
// down and right, darkening what is under it.
static const char* const arrow[PTR_H - PTR_SHADOW] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.....XXXXX",
    "X..X..X",
    "X.X X..X",
    "XX  X..X",
    "X    X..X",
    "     X..X",
    "      XX",
};

static void pointer(void) {
    for (int pass = 0; pass < 2; pass++)  // the shadow, then the arrow on it
        for (int r = 0; r < PTR_H - PTR_SHADOW; r++)
            for (int c = 0; arrow[r][c]; c++) {
                char k = arrow[r][c];
                int x = mouse.x + c + (pass ? 0 : PTR_SHADOW), y = mouse.y + r + (pass ? 0 : PTR_SHADOW);
                if (k == ' ' || x < 0 || x >= W || y < 0 || y >= H) continue;
                uint32_t* p = &frame[y * W + x];
                if (!pass) *p = (*p >> 1) & 0x7F7F7F;  // half as bright
                else *p = k == 'X' ? 0x000000 : 0xFFFFFF;
            }
}

static void clock(void) {
    rtc_time_t t;
    char buf[24];
    if (sys_time(&t) != 0) return;
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", t.year, t.month, t.day, t.hour, t.min);
    if (strcmp(buf, wm.clock)) wm_set_clock(&wm, buf);
}

static void draw(void) {
    wm_rect_t d[WM_DAMAGE_MAX];
    int n = wm_take_damage(&wm, d);
    for (int i = 0; i < n; i++) {
        wm_compose(&wm, d[i]);
        pointer();
        gfx_put_t p = { d[i].x, d[i].y, d[i].w, d[i].h, W, 0, frame + d[i].y * W + d[i].x };
        sys_gfx_put(&p);
    }
}

int main(int argc, char** argv) {
    listener = sys_chan_listen(WS_CHANNEL);
    if (listener < 0) {
        printf("pleroma: cannot listen on %s: %s\n", WS_CHANNEL,
               listener == -EADDRINUSE ? "a window server is running already" : strerror(listener));
        return 1;
    }
    int r = sys_gfx_begin_native();
    if (r < 0) {
        printf("pleroma: cannot take the display: %s\n", strerror(r));
        return 1;
    }
    gfx_info_t gi;
    sys_gfx_info(&gi);
    W = gi.width, H = gi.height;
    frame = malloc((size_t)W * H * 4);
    if (!frame || W < 320 || H < 200) {
        sys_gfx_end();
        printf("pleroma: no room for a %dx%d screen\n", W, H);
        return 1;
    }
    // Ctrl+C and Ctrl+backslash are keys for the window that has the
    // focus (a terminal's program, say), not signals for the desktop.
    // Ctrl+Alt+Backspace leaves; three Ctrl+backslashes in two seconds
    // still end it, whatever it ignores.
    sys_tty_mode(TTY_NOSIG);
    signal(SIGQUIT, SIG_IGN);
    sys_gfx_font(font);
    wm_init(&wm, W, H, frame, font);
    uint32_t top, bottom;
    if (desk_colours_load(&top, &bottom) == 0) wm_set_desktop(&wm, top, bottom);
    for (int c = 0; c < MAX_CLIENTS; c++) clients[c].fd = -1, clients[c].win = -1;
    sys_mouse_state(&mouse);
    clock();
    for (int i = 1; i < argc; i++) start(argv[i]);
    if (argc < 2) start("KENOMA");

    while (!quit) {
        draw();
        pollfd_t p[2 + MAX_CLIENTS];
        int who[2 + MAX_CLIENTS], n = 0;
        p[n] = (pollfd_t){ POLLFD_INPUT, POLLIN, 0 }, who[n++] = -1;
        p[n] = (pollfd_t){ listener, POLLIN, 0 }, who[n++] = -2;
        for (int c = 0; c < MAX_CLIENTS; c++)
            if (clients[c].fd >= 0) p[n] = (pollfd_t){ clients[c].fd, POLLIN, 0 }, who[n++] = c;
        if (sys_pollfd(p, n, 1000) < 0) break;
        input();
        if (p[1].revents & POLLIN) accept_client();
        for (int i = 2; i < n; i++)
            if ((p[i].revents & (POLLIN | POLLHUP)) && clients[who[i]].fd >= 0 && !read_client(who[i])) drop(who[i]);
        focus_changed();
        clock();
    }

    for (int c = 0; c < MAX_CLIENTS; c++)
        if (clients[c].fd >= 0) drop(c);  // they read the end
    sys_close(listener);
    sys_gfx_end();
    return 0;
}

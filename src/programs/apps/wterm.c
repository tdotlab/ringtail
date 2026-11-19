// wterm: a terminal in a window on pleroma's desktop. It makes a pty,
// starts the shell (or the program named on its command line) with the
// pty as its terminal, draws what the program writes with the console's
// own emulator (vt.h) and font, and types the window's keys into it.
//
//     wterm                  saklas in an 80x24 window
//     wterm octavo notes.txt anything else that runs on a terminal
//
// The window closes when the program ends; closing the window hangs the
// pty up, which ends the program (SIGHUP).
#include "ulib.h"
#include "win.h"
#include "keymap.h"
#include "vt.h"

#define COLS    80
#define ROWS    24
#define GW      8
#define GH      16
#define DEFAULT 0x07  // light grey on black, as a terminal starts

static win_t w;
static vt_t vt;
static uint16_t cells[COLS * ROWS];
static uint16_t shown[COLS * ROWS];
static int shown_cursor = -1, shown_focus = -1, shown_ok;
static uint8_t font[256 * GH];

// The 16 VGA colours, as the console draws them.
static const uint32_t pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// One cell. The cursor is a bar across its bottom two rows, as on the
// console, or, while another window has the keys, a hollow box.
static void draw_cell(int i, uint16_t v, int cursor) {
    const uint8_t* g = &font[(v & 0xFF) * GH];
    uint32_t fg = pal[(v >> 8) & 0x0F], bg = pal[(v >> 12) & 0x0F];
    int x0 = (i % COLS) * GW, y0 = (i / COLS) * GH;
    for (int r = 0; r < GH; r++) {
        uint8_t bits = g[r];
        if (cursor && w.focused && r >= GH - 2) bits = 0xFF;
        if (cursor && !w.focused) bits |= (r == 0 || r == GH - 1) ? 0xFF : 0x81;
        uint32_t* p = &w.pixels[(y0 + r) * w.w + x0];
        for (int c = 0; c < GW; c++) p[c] = bits & (0x80 >> c) ? fg : bg;
    }
}

// The cells that changed since last time, and the cursor, drawn and
// sent to the server as one rectangle.
static void redraw(void) {
    int cur = vt.cursor_hidden ? -1 : vt.row * COLS + vt.col;
    int focus_changed = shown_focus != w.focused;
    int lo = COLS * ROWS, hi = -1;
    for (int i = 0; i < COLS * ROWS; i++) {
        int is_cur = i == cur, was_cur = i == shown_cursor;
        int same = shown_ok && cells[i] == shown[i] && is_cur == was_cur;
        if (same && !(is_cur && focus_changed)) continue;
        draw_cell(i, cells[i], is_cur);
        shown[i] = cells[i];
        if (lo > i) lo = i;
        hi = i;
    }
    shown_cursor = cur;
    shown_focus = w.focused;
    shown_ok = 1;
    if (hi < 0) return;
    int r0 = lo / COLS, r1 = hi / COLS;
    int c0 = r0 == r1 ? lo % COLS : 0, c1 = r0 == r1 ? hi % COLS : COLS - 1;
    win_damage(&w, c0 * GW, r0 * GH, (c1 - c0 + 1) * GW, (r1 - r0 + 1) * GH);
}

static void say(const char* s) { vt_write(&vt, s, strlen(s)); }

// A key for the terminal: what the console's keyboard would put in
// its queue, ESC first for Alt.
static void type(int master, const ws_msg_t* ev) {
    if (!ev->key.down || !ev->key.code) return;
    char b[2];
    int n = 0;
    if ((ev->key.mods & KEY_MOD_ALT) && !KEY_IS_NAV(ev->key.code)) b[n++] = 0x1B;
    b[n++] = (char)ev->key.code;
    sys_write(master, b, (uint32_t)n);
}

int main(int argc, char** argv) {
    int e = win_open(&w, COLS * GW, ROWS * GH, "Terminal");
    if (e < 0) {
        dprintf(STDERR_FILENO, "wterm: no window: %s\n", strerror(-e));
        return 1;
    }
    if (sys_gfx_font(font) < 0) memset(font, 0, sizeof font);
    vt_init(&vt, cells, COLS, ROWS, DEFAULT);

    int fds[2];
    tty_size_t size = { COLS, ROWS };
    int master = -1, child = -1;
    e = sys_pty_open(fds, &size);
    if (e < 0) {
        say("wterm: no pty: ");
        say(strerror(-e));
    } else {
        master = fds[0];
        const char* shell[] = { "SAKLAS", 0 };
        const char* const* args = argc > 1 ? (const char* const*)argv + 1 : shell;
        spawn_opts_t o = { SPAWN_NEW_JOB | SPAWN_FOREGROUND, 0, { fds[1], fds[1], fds[1] }, -1 };
        child = sys_spawn_opts(args[0], args, &o);
        sys_close(fds[1]);  // the program has it now; when it is done, the master reads end of file
        if (child < 0) {
            say("wterm: cannot start ");
            say(args[0]);
            say(": ");
            say(strerror(-child));
            sys_close(master);
            master = -1;
        }
    }
    vt.cursor_hidden |= master < 0;
    redraw();

    static char buf[4096];
    for (;;) {
        pollfd_t p[2] = { { w.fd, POLLIN, 0 }, { master, POLLIN, 0 } };
        int n = sys_pollfd(p, master >= 0 ? 2 : 1, -1);
        if (n < 0 && n != -EINTR) break;
        if (master >= 0 && p[1].revents) {
            // Take what there is, up to a screenful or so, before drawing.
            int got = 0, r;
            do {
                r = sys_read(master, buf, sizeof buf);
                if (r > 0) vt_write(&vt, buf, (size_t)r);
                got += r > 0 ? r : 0;
                pollfd_t q = { master, POLLIN, 0 };
                if (r <= 0 || got >= 16384 || sys_pollfd(&q, 1, 0) != 1) break;
            } while (1);
            if (r <= 0) break;  // the program is done
        }
        ws_msg_t ev;
        int closing = 0;
        while (win_event(&w, &ev, 0) > 0) {
            if (ev.type == WS_KEY && master >= 0) type(master, &ev);
            if (ev.type == WS_CLOSE) closing = 1;
        }
        if (closing || (p[0].revents & (POLLHUP | POLLERR))) break;
        redraw();
    }
    if (master >= 0) sys_close(master);  // hangs up: anything still running gets SIGHUP
    win_close(&w);
    return 0;
}

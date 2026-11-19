// The window system's client side (win.h).
#include "ulib.h"
#include "win.h"

static void send(win_t* win, const ws_msg_t* m) { sys_write(win->fd, m, sizeof *m); }

static void copy_title(char* d, const char* s) {
    strlcpy(d, s ? s : "", WS_TITLE_MAX);
}

int win_open(win_t* win, int w, int h, const char* title) {
    memset(win, 0, sizeof *win);
    win->fd = -1;
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h * 4 > SHM_MAX) return -EINVAL;
    int fd = sys_chan_connect(WS_CHANNEL);
    if (fd < 0) return fd;
    shm_info_t in;
    int id = sys_shm_new((uint32_t)(w * h * 4), &in);
    if (id < 0) {
        sys_close(fd);
        return id;
    }
    win->fd = fd;
    win->w = w;
    win->h = h;
    win->pixels = in.addr;
    win->shm = (uint32_t)id;
    for (int i = 0; i < w * h; i++) win->pixels[i] = 0xFFFFFF;

    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_HELLO;
    m.hello.pid = sys_getpid();
    send(win, &m);
    memset(&m, 0, sizeof m);
    m.type = WS_CREATE;
    m.create.shm = win->shm;
    m.create.w = w;
    m.create.h = h;
    copy_title(m.create.title, title);
    send(win, &m);
    // The answer comes before any event.
    int r = win_event(win, &m, 5000);
    if (r > 0 && m.type == WS_SHOWN && m.shown.error == 0) return 0;
    win_close(win);
    if (r == 0) return -ETIMEDOUT;
    if (r < 0) return r;
    return m.type == WS_SHOWN && m.shown.error < 0 ? m.shown.error : -EIO;  // not the answer the protocol says comes
}

void win_damage(win_t* win, int x, int y, int w, int h) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(win->w, x + w), y1 = MIN(win->h, y + h);
    if (win->fd < 0 || x1 <= x0 || y1 <= y0) return;
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_DAMAGE;
    m.damage.x = x0;
    m.damage.y = y0;
    m.damage.w = x1 - x0;
    m.damage.h = y1 - y0;
    send(win, &m);
}

void win_title(win_t* win, const char* title) {
    if (win->fd < 0) return;
    ws_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = WS_TITLE;
    copy_title(m.title.title, title);
    send(win, &m);
}

int win_event(win_t* win, ws_msg_t* ev, int timeout_ms) {
    if (win->fd < 0) return -EPIPE;
    pollfd_t p = { win->fd, POLLIN, 0 };
    int r = sys_pollfd(&p, 1, timeout_ms);
    if (r < 0) return r;
    if (r == 0) return 0;
    memset(ev, 0, sizeof *ev);
    int n = sys_read(win->fd, ev, sizeof *ev);
    if (n <= 0) return n == -EINTR ? n : -EPIPE;
    if (ev->type == WS_FOCUS) win->focused = ev->focus.on != 0;
    return 1;
}

void win_close(win_t* win) {
    if (win->fd >= 0) sys_close(win->fd);
    if (win->pixels) sys_shm_unmap((uintptr_t)win->pixels);
    win->fd = -1;
    win->pixels = 0;
}

// ---- Drawing ----

void win_fill(win_t* win, int x, int y, int w, int h, uint32_t color) {
    int x0 = MAX(0, x), y0 = MAX(0, y), x1 = MIN(win->w, x + w), y1 = MIN(win->h, y + h);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) win->pixels[yy * win->w + xx] = color;
}

static uint8_t font[256 * 16];
static int font_loaded;

int win_text(win_t* win, int x, int y, const char* s, uint32_t color) {
    if (!font_loaded) font_loaded = sys_gfx_font(font) == 0;
    for (; *s; s++, x += 8) {
        const uint8_t* g = &font[(uint8_t)*s * 16];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) {
                    int px = x + col, py = y + row;
                    if (px >= 0 && px < win->w && py >= 0 && py < win->h) win->pixels[py * win->w + px] = color;
                }
    }
    return x;
}

void win_line(win_t* win, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < win->w && y0 >= 0 && y0 < win->h) win->pixels[y0 * win->w + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

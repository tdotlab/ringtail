// A clock in a window on pleroma's desktop: a face with hands, and the
// time in figures under it, redrawn each second. Closes when asked.
#include "ulib.h"
#include "win.h"

#define SIZE 200   // the face's square
#define TEXT_H 28  // the figures under it

// sin of 6 degrees steps, times 1000, for a quarter turn.
static const int sin_q[16] = { 0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000 };

// A point len pixels from the centre, toward minute mark m (0-59).
static void toward(int m, int len, int* x, int* y) {
    m %= 60;
    int q = m / 15, i = m % 15;
    int s = sin_q[i], c = sin_q[15 - i];
    int dx, dy;  // screen y grows down, so up is -c
    switch (q) {
    case 0: dx = s, dy = -c; break;
    case 1: dx = c, dy = s; break;
    case 2: dx = -s, dy = c; break;
    default: dx = -c, dy = -s; break;
    }
    *x = SIZE / 2 + dx * len / 1000;
    *y = SIZE / 2 + dy * len / 1000;
}

static void hand(win_t* w, int m, int len, int thick, uint32_t color) {
    int x, y;
    toward(m, len, &x, &y);
    for (int a = -thick / 2; a <= thick / 2; a++)
        for (int b = -thick / 2; b <= thick / 2; b++) win_line(w, SIZE / 2 + a, SIZE / 2 + b, x + a, y + b, color);
}

static void draw(win_t* w, const rtc_time_t* t) {
    win_fill(w, 0, 0, w->w, w->h, 0xF4F5F7);
    // The face: a disc, drawn row by row.
    int r = SIZE / 2 - 8;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) w->pixels[(SIZE / 2 + y) * w->w + SIZE / 2 + x] = 0xFFFFFF;
    for (int m = 0; m < 60; m++) {
        int x0, y0, x1, y1;
        toward(m, r - (m % 5 ? 4 : 12), &x0, &y0);
        toward(m, r - 1, &x1, &y1);
        win_line(w, x0, y0, x1, y1, m % 5 ? 0x9AA2AE : 0x1C2430);
    }
    hand(w, (int)(t->hour % 12) * 5 + (int)t->min / 12, r * 5 / 10, 4, 0x1C2430);
    hand(w, (int)t->min, r * 8 / 10, 2, 0x1C2430);
    hand(w, (int)t->sec, r * 9 / 10, 0, 0xC0392B);
    char buf[16];
    snprintf(buf, sizeof buf, "%02u:%02u:%02u", t->hour, t->min, t->sec);
    win_text(w, (w->w - (int)strlen(buf) * 8) / 2, SIZE + (TEXT_H - 16) / 2 - 2, buf, 0x1C2430);
    win_damage(w, 0, 0, w->w, w->h);
}

int main(void) {
    win_t w;
    int r = win_open(&w, SIZE, SIZE + TEXT_H, "Clock");
    if (r < 0) {
        printf("wclock: no window: %s\n", strerror(r));
        return 1;
    }
    rtc_time_t t, shown;
    memset(&shown, 0xFF, sizeof shown);
    for (;;) {
        if (sys_time(&t) == 0 && t.sec != shown.sec) {
            draw(&w, &t);
            shown = t;
        }
        ws_msg_t ev;
        int e = win_event(&w, &ev, 250);
        if (e < 0 || (e > 0 && ev.type == WS_CLOSE)) break;
    }
    win_close(&w);
    return 0;
}

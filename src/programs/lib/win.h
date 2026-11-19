// A window on pleroma's desktop, for a program (the client side of
// wsys.h). Open one, draw into its pixels, say what changed, and read
// events:
//
//     win_t w;
//     if (win_open(&w, 200, 100, "Hello") < 0) return 1;
//     win_fill(&w, 0, 0, 200, 100, 0xFFFFFF);
//     win_text(&w, 10, 10, "hello", 0);
//     win_damage(&w, 0, 0, 200, 100);
//     ws_msg_t ev;
//     while (win_event(&w, &ev, -1) > 0)
//         if (ev.type == WS_CLOSE) break;
//     win_close(&w);
#ifndef RINGTAIL_WIN_H
#define RINGTAIL_WIN_H

#include "wsys.h"

typedef struct {
    int fd;            // the channel to the server
    int w, h;
    uint32_t* pixels;  // w*h, 0xRRGGBB, rows packed
    uint32_t shm;
    int focused;       // kept up to date by win_event
} win_t;

// Connects, makes the pixels (white) and asks for the window. 0 when it
// is up; -ECONNREFUSED when no server runs, -ETIMEDOUT or -EIO when it
// does not answer as it should, or what the server or the
// kernel refused with.
int win_open(win_t* win, int w, int h, const char* title);
// The pixels in x, y, w, h changed; clipped to the window.
void win_damage(win_t* win, int x, int y, int w, int h);
void win_title(win_t* win, const char* title);
// The next event in *ev: 1; 0 when timeout_ms (negative: no limit)
// passed first; -EPIPE when the server has gone.
int win_event(win_t* win, ws_msg_t* ev, int timeout_ms);
void win_close(win_t* win);

// Drawing into the pixels, clipped to the window. Nothing shows until
// win_damage.
void win_fill(win_t* win, int x, int y, int w, int h, uint32_t color);
// 8x16 characters, the console's font; returns the x after the text.
int win_text(win_t* win, int x, int y, const char* s, uint32_t color);
void win_line(win_t* win, int x0, int y0, int x1, int y1, uint32_t color);

#endif

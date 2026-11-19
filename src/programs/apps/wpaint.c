// Paint, in a window on pleroma's desktop: a row of colours and a
// Clear button over a canvas. Drag with the left button to draw, with
// the right to rub out; keys 1 to 8 pick a colour, C clears. Closes
// when asked.
#include "ulib.h"
#include "win.h"

#define WIDTH  420
#define HEIGHT 320
#define TOOLS  28  // the strip at the top
#define SWATCH 24
#define ABS(v) ((v) < 0 ? -(v) : (v))

static const uint32_t colours[8] = {
    0x1C2430, 0xC0392B, 0xE67E22, 0xF1C40F, 0x27AE60, 0x2E86DE, 0x8E44AD, 0xFFFFFF
};
static int pick;
static win_t w;

static void tools(void) {
    win_fill(&w, 0, 0, WIDTH, TOOLS, 0xD8DCE2);
    for (int i = 0; i < 8; i++) {
        int x = 4 + i * (SWATCH + 4);
        win_fill(&w, x - 2, 0 + 0, SWATCH + 4, TOOLS, i == pick ? 0x2E5C9A : 0xD8DCE2);
        win_fill(&w, x, 2, SWATCH, SWATCH, colours[i]);
    }
    int cx = WIDTH - 64;
    win_fill(&w, cx, 3, 60, TOOLS - 6, 0x5A6472);
    win_text(&w, cx + 10, 6, "Clear", 0xFFFFFF);
    win_fill(&w, 0, TOOLS - 1, WIDTH, 1, 0x9AA2AE);
    win_damage(&w, 0, 0, WIDTH, TOOLS);
}

static void clear(void) {
    win_fill(&w, 0, TOOLS, WIDTH, HEIGHT - TOOLS, 0xFFFFFF);
    win_damage(&w, 0, TOOLS, WIDTH, HEIGHT - TOOLS);
}

// A line of squares of the brush's size, kept off the tool strip.
static void stroke(int x0, int y0, int x1, int y1, int size, uint32_t c) {
    int dx = ABS(x1 - x0), dy = ABS(y1 - y0), n = MAX(dx, dy);
    for (int i = 0; i <= n; i++) {
        int x = n ? x0 + (x1 - x0) * i / n : x0, y = n ? y0 + (y1 - y0) * i / n : y0;
        int top = MAX(TOOLS, y - size / 2);
        win_fill(&w, x - size / 2, top, size, y - size / 2 + size - top, c);
    }
    int x = MIN(x0, x1) - size, y = MIN(y0, y1) - size;
    win_damage(&w, x, y, dx + 2 * size + 1, dy + 2 * size + 1);
}

int main(void) {
    int r = win_open(&w, WIDTH, HEIGHT, "Paint");
    if (r < 0) {
        printf("wpaint: no window: %s\n", strerror(r));
        return 1;
    }
    tools();
    clear();
    int lx = 0, ly = 0, held = 0, on_strip = 0;
    ws_msg_t ev;
    while (win_event(&w, &ev, -1) > 0) {
        if (ev.type == WS_CLOSE) break;
        if (ev.type == WS_KEY && ev.key.down) {
            int c = ev.key.code;
            if (c >= '1' && c <= '8') pick = c - '1', tools();
            if (c == 'c' || c == 'C') clear();
        }
        if (ev.type != WS_MOUSE) continue;
        int x = ev.mouse.x, y = ev.mouse.y, b = ev.mouse.buttons;
        int size = b & 2 ? 16 : 4;
        uint32_t c = b & 2 ? 0xFFFFFF : colours[pick];
        if (b && !held) {  // a press: on the strip, or the start of a stroke
            on_strip = y < TOOLS;
            if (on_strip) {
                int i = (x - 2) / (SWATCH + 4);
                if (x >= WIDTH - 64 && x < WIDTH - 4) clear();
                else if (x >= 2 && i < 8) pick = i, tools();
            } else {
                stroke(x, y, x, y, size, c);
            }
        } else if (b && !on_strip) {
            stroke(lx, ly, x, y, size, c);
        }
        lx = x, ly = y, held = b;
    }
    win_close(&w);
    return 0;
}

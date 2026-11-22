// Settings: a window on pleroma's desktop for the few things the system
// lets a user set. The date and time: typed in and set, or asked of a
// time server (sntp, in a terminal window). The desktop's colours: a
// choice of a few, used at once and kept in the file desk.h names, which
// pleroma reads when it starts. And what the network is doing, from
// SYS_NETCONF, read again when the window gets the keys or on Refresh;
// changing it is ifconfig's job. At the bottom, the system's version.
#include "ulib.h"
#include "win.h"
#include "tk.h"
#include "desk.h"

#define PAD 8

typedef struct {
    const char* name;
    uint32_t top, bottom;
} preset_t;

static const preset_t presets[] = {
    { "Deep sea", 0x102A5C, 0x146E80 }, { "Dusk", 0x2B1B4A, 0xC0605A }, { "Forest", 0x0F2A1D, 0x3E7B4F },
    { "Slate", 0x2A2F38, 0x5A6472 },    { "Sand", 0x8A6F4E, 0xE8D6B0 }, { "Plum", 0x3A1240, 0x7A3E7E },
};
#define NPRESETS (int)(sizeof presets / sizeof presets[0])
static tk_item_t items[NPRESETS];

static win_t win;
static tk_t tk;
static int clock_now, date, time_, set, sync_, colours, net[4], refresh_btn;

static void info(const char* text) {
    static const char* const ok[] = { "OK" };
    tk_dialog(&tk, 0, text, ok, 1, 0);
}

static void show_clock(int fields) {
    rtc_time_t t;
    char buf[TK_TEXT_MAX];
    if (sys_time(&t) < 0) {
        tk_set_text(&tk, clock_now, "The clock cannot be read.");
        return;
    }
    snprintf(buf, sizeof buf, "The clock says %04u-%02u-%02u %02u:%02u:%02u.", t.year, t.month, t.day, t.hour, t.min,
             t.sec);
    tk_set_text(&tk, clock_now, buf);
    if (!fields) return;
    snprintf(buf, sizeof buf, "%04u-%02u-%02u", t.year, t.month, t.day);
    tk_set_text(&tk, date, buf);
    snprintf(buf, sizeof buf, "%02u:%02u:%02u", t.hour, t.min, t.sec);
    tk_set_text(&tk, time_, buf);
}

// "n" digits and then sep (or the end, for sep 0): the number, or -1.
static int number(const char** s, int n, char sep) {
    int v = 0;
    for (int i = 0; i < n; i++, (*s)++) {
        if (**s < '0' || **s > '9') return -1;
        v = v * 10 + (**s - '0');
    }
    if (**s != sep) return -1;
    if (sep) (*s)++;
    return v;
}

static void set_clock(void) {
    const char* d = tk_text(&tk, date);
    const char* t = tk_text(&tk, time_);
    int y = number(&d, 4, '-'), mo = number(&d, 2, '-'), dd = number(&d, 2, 0);
    int h = number(&t, 2, ':'), mi = number(&t, 2, ':'), s = number(&t, 2, 0);
    if (y < 0 || mo < 0 || dd < 0 || h < 0 || mi < 0 || s < 0) {
        info("Type the date as YYYY-MM-DD and\nthe time as HH:MM:SS.");
        return;
    }
    rtc_time_t rt = { (unsigned)y, (unsigned)mo, (unsigned)dd, (unsigned)h, (unsigned)mi, (unsigned)s };
    int r = sys_settime(&rt);
    if (r == -EINVAL) info("There is no such date or time.");
    else if (r == -ERANGE) info("The clock cannot be set before this\nsystem was built, or after 2099.");
    else if (r < 0) {
        char text[TK_DLG_MAX];
        snprintf(text, sizeof text, "The clock was not set:\n%s.", strerror(r));
        info(text);
    }
    show_clock(1);
}

static void show_network(void) {
    netconf_t c;
    char buf[4][TK_TEXT_MAX];
    if (sys_netconf(NETCONF_GET, &c) < 0) {
        tk_set_text(&tk, net[0], "No network.");
        for (int i = 1; i < 4; i++) tk_set_text(&tk, net[i], "");
        return;
    }
    int bits = 0;
    for (unsigned m = c.mask; m & 0x80000000u; m <<= 1) bits++;
    const char* how = c.mode == NETCONF_DHCP ? "from DHCP" : "set by hand";
    if (c.ip) snprintf(buf[0], sizeof buf[0], "Address  " IPV4_FMT "/%d, %s", IPV4_ARGS(c.ip), bits, how);
    else snprintf(buf[0], sizeof buf[0], "Address  none yet, %s", how);
    if (c.gw) snprintf(buf[1], sizeof buf[1], "Gateway  " IPV4_FMT, IPV4_ARGS(c.gw));
    else snprintf(buf[1], sizeof buf[1], "Gateway  none");
    if (c.dns[0] && c.dns[1])
        snprintf(buf[2], sizeof buf[2], "DNS      " IPV4_FMT ", " IPV4_FMT, IPV4_ARGS(c.dns[0]), IPV4_ARGS(c.dns[1]));
    else if (c.dns[0]) snprintf(buf[2], sizeof buf[2], "DNS      " IPV4_FMT, IPV4_ARGS(c.dns[0]));
    else snprintf(buf[2], sizeof buf[2], "DNS      none");
    if (c.mode == NETCONF_DHCP && c.ip && c.lease >= 0)
        snprintf(buf[3], sizeof buf[3], "Lease    %d:%02d left%s%s", c.lease / 3600, c.lease / 60 % 60,
                 c.domain[0] ? ", domain " : "", c.domain);
    else snprintf(buf[3], sizeof buf[3], "Domain   %s", c.domain[0] ? c.domain : "none");
    for (int i = 0; i < 4; i++) tk_set_text(&tk, net[i], buf[i]);
}

static void use_colours(int i) {
    if (i < 0 || i >= NPRESETS) return;
    win_desktop(&win, presets[i].top, presets[i].bottom);
    int r = desk_colours_save(presets[i].top, presets[i].bottom);
    if (r < 0) {
        char text[TK_DLG_MAX];
        snprintf(text, sizeof text, "The colours are used, but they could\nnot be kept for next time:\n%s.",
                 strerror(r));
        info(text);
    }
}

int main(void) {
    int ww = 470, wh = 352;
    int r = win_open(&win, ww, wh, "Settings");
    if (r < 0) {
        printf("wprefs: no window: %s\n", strerror(r));
        return 1;
    }
    tk_init(&tk, win.pixels, win.w, win.h, win_font());

    int y = PAD;
    tk_label(&tk, PAD, y, 200, "Date and time");
    clock_now = tk_label(&tk, 160, y, ww - 160 - PAD, "");
    y += 22;
    date = tk_field(&tk, PAD, y, 104, "");
    time_ = tk_field(&tk, PAD + 112, y, 84, "");
    set = tk_button(&tk, PAD + 204, y, 72, "Set");
    sync_ = tk_button(&tk, PAD + 284, y, 170, "Ask a time server");
    show_clock(1);

    y += TK_LINE_H + 16;
    tk_label(&tk, PAD, y, 200, "Desktop colours");
    y += 22;
    for (int i = 0; i < NPRESETS; i++) items[i] = (tk_item_t){ presets[i].name, 0, TK_ICON_NONE };
    colours = tk_list(&tk, PAD, y, 200, NPRESETS * TK_ROW_H + 2, TK_ROWS);
    tk_list_set(&tk, colours, items, NPRESETS);
    uint32_t top, bottom;
    if (desk_colours_load(&top, &bottom) < 0) top = presets[0].top, bottom = presets[0].bottom;  // pleroma's own
    for (int i = 0; i < NPRESETS; i++)
        if (presets[i].top == top && presets[i].bottom == bottom) tk_list_select(&tk, colours, i);
    tk_label(&tk, PAD + 216, y, ww - 216 - 2 * PAD, "Chosen colours are used\nat once, and kept for\nthe next time the\ndesktop starts.");

    y += NPRESETS * TK_ROW_H + 2 + 16;
    tk_label(&tk, PAD, y, 200, "Network");
    refresh_btn = tk_button(&tk, ww - PAD - 88, y - 4, 88, "Refresh");
    y += 22;
    for (int i = 0; i < 4; i++) net[i] = tk_label(&tk, PAD, y + i * 18, ww - 2 * PAD, "");
    show_network();

    char v[80];
    if (sys_kinfo(KINFO_VERSION, v, sizeof v) < 0) strlcpy(v, "Ringtail", sizeof v);
    char* nl = strchr(v, '\n');
    if (nl) *nl = 0;
    tk_label(&tk, PAD, wh - 16 - PAD, ww - 2 * PAD, v);
    tk_set_focus(&tk, colours);

    for (;;) {
        tk_rect_t d = tk_draw(&tk);
        if (d.w) win_damage(&win, d.x, d.y, d.w, d.h);
        ws_msg_t m;
        r = win_event(&win, &m, 1000);
        if (r < 0) break;
        if (r == 0) {  // a second: the clock moves on
            show_clock(0);
            continue;
        }
        if (m.type == WS_CLOSE) break;
        if (m.type == WS_FOCUS && m.focus.on) show_network();
        tk.now = sys_ticks();
        tk_event_t e;
        if (!tk_handle(&tk, &m, &e)) continue;
        if (e.type == TK_EV_SELECT && e.id == colours) use_colours(e.a);
        else if (e.type == TK_EV_CLICK && e.id == set) set_clock();
        else if (e.type == TK_EV_SUBMIT && (e.id == date || e.id == time_)) set_clock();
        else if (e.type == TK_EV_CLICK && e.id == refresh_btn) show_network();
        else if (e.type == TK_EV_CLICK && e.id == sync_) {
            r = desk_start("SNTP");
            if (r < 0) {
                char text[TK_DLG_MAX];
                snprintf(text, sizeof text, "sntp cannot be started:\n%s.", strerror(r));
                info(text);
            }
        }
    }
    win_close(&win);
    return 0;
}

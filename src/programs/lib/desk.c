// The desktop's shared bits (desk.h).
#include "ulib.h"
#include "desk.h"

const desk_app_t desk_apps[] = {
    { "WFILES", "Files" },
    { "WTERM", "Terminal" },
    { "WCLOCK", "Clock" },
    { "WPAINT", "Paint" },
    { "WPREFS", "Settings" },
};
const int desk_napps = sizeof desk_apps / sizeof desk_apps[0];

const desk_app_t* desk_app(const char* prog) {
    for (int i = 0; i < desk_napps; i++)
        if (strcasecmp(prog, desk_apps[i].prog) == 0) return &desk_apps[i];
    return 0;
}

int desk_hidden(const char* prog) { return strcasecmp(prog, "KENOMA") == 0 || strcasecmp(prog, "PLEROMA") == 0; }

int desk_document(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    dot++;
    return strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "lem") == 0 || strcasecmp(dot, "lemur") == 0;
}

int desk_runnable(const char* path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char head[4] = { 0 };
    int n = sys_read(fd, head, sizeof head);
    sys_close(fd);
    return (n == 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') ||
           (n >= 2 && head[0] == '#' && head[1] == '!');
}

static int spawn(const char* const* argv) {
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { -1, -1, -1 }, -1 };
    return sys_spawn_opts(argv[0], argv, &o);
}

int desk_start(const char* prog) {
    if (desk_app(prog)) {
        const char* argv[] = { prog, 0 };
        return spawn(argv);
    }
    const char* argv[] = { "WTERM", "-h", prog, 0 };
    return spawn(argv);
}

int desk_open_document(const char* path) {
    const char* argv[] = { "WTERM", "OCTAVO", path, 0 };
    return spawn(argv);
}

static int hex(const char** s, uint32_t* v) {
    while (**s == ' ' || **s == '\t') (*s)++;
    int n = 0;
    *v = 0;
    for (;; (*s)++, n++) {
        char c = **s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        if (n == 6) return -1;
        *v = *v << 4 | (uint32_t)d;
    }
    return n ? 0 : -1;
}

int desk_colours_load(uint32_t* top, uint32_t* bottom) {
    int fd = sys_open(DESK_COLOURS, O_RDONLY);
    if (fd < 0) return fd;
    char buf[64];
    int n = sys_read(fd, buf, sizeof buf - 1);
    sys_close(fd);
    if (n < 0) return n;
    buf[n] = 0;
    const char* s = buf;
    uint32_t t, b;
    if (hex(&s, &t) < 0 || hex(&s, &b) < 0) return -EINVAL;
    *top = t, *bottom = b;
    return 0;
}

int desk_colours_save(uint32_t top, uint32_t bottom) {
    int fd = sys_open(DESK_COLOURS, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fd;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%06X %06X\n", top & 0xFFFFFF, bottom & 0xFFFFFF);
    int w = sys_write(fd, buf, (uint32_t)n);
    sys_close(fd);
    return w == n ? 0 : w < 0 ? w : -EIO;
}

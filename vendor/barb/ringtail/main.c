// barb for RingTail: the command line, the terminal, and the loop that joins the agent to the system.
//
//   barb                       an interactive session (type a task; /help for commands)
//   barb -p "task"             do one task and exit; the answer goes to standard output
//
// RingTail programs have no environment, so the key, the endpoint and the model come from a file,
// /barb.cfg by default ("key = ...", "url = ...", "model = ...", "ca = ..."; # starts a comment).
// The options override it.
//
// The terminal is in raw mode with Ctrl-C delivered as a key (TTY_RAW | TTY_NOSIG), and this file
// edits the line itself. It has to: in cooked mode a read waits for the whole line, and the loop
// must keep serving the agent (a reply arriving, a task ending) while someone types. The line
// being typed is drawn under the output and moved down as output arrives. The prompt shows only
// when the agent is idle, or once something has been typed.
//
// Like the Linux version, the loop sleeps in sys_pollfd when nothing is happening. The one
// difference: RingTail tells no one when a program ends, so while one runs, the loop looks every
// 50 ms (250 ms for background tasks). That costs a system call, never a model call.
#include "ulib.h"
#include "../core/agent.h"
#include "ops.h"

#define VERSION "0.2.0"
#define DEFAULT_MODEL "claude-sonnet-5-5"
#define DEFAULT_URL "https://api.anthropic.com"
#define CONFIG "/barb.cfg"

// ---- events waiting for the agent ----

typedef struct qev {
    struct qev* next;
    ev_t ev;
    char* data;
    char* note;
} qev_t;

static qev_t *qhead, *qtail;

static char* dup_n(const char* p, size_t n) {
    char* s = malloc(n + 1);
    if (!s) return 0;
    if (n) memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static void deliver(const ev_t* ev) {
    qev_t* q = malloc(sizeof *q);
    if (!q) return;
    memset(q, 0, sizeof *q);
    q->ev = *ev;
    if (ev->data.p) { q->data = dup_n(ev->data.p, ev->data.n); q->ev.data = str_of(q->data, q->data ? ev->data.n : 0); }
    if (ev->note.p) { q->note = dup_n(ev->note.p, ev->note.n); q->ev.note = str_z(q->note); }
    if (qtail) qtail->next = q; else qhead = q;
    qtail = q;
}

static void deliver_simple(evk_t kind, int64_t id, int status, const char* data) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = id;
    e.status = status;
    if (data) e.data = str_z(data);
    deliver(&e);
}

// ---- state ----

static void* m_alloc(size_t n) { return malloc(n); }
static void m_release(void* p) { free(p); }
static const mem_t mem = { m_alloc, m_release };

static agent_t* agent;
static int done, exit_code;
static int64_t awaiting;        // the question the user is being asked, if any
static int interactive;         // the terminal is ours: raw mode, the line editor
static int tty_out;             // standard error is a terminal: colours, the waiting line
static int oneshot;             // -p
static int stdin_open = 1;
static int old_mode = -1;       // the terminal's mode before ours
static volatile int got_sigint, got_sigterm;

static void on_sigint(int s) { (void)s; got_sigint = 1; }
static void on_sigterm(int s) { (void)s; got_sigterm = 1; }

// ---- the screen ----

static char ed[4096];           // the line being typed
static int edn;
static int ed_shown;            // the prompt and that line are on the screen
static int64_t http_id, http_since;
static int spin_shown;
static int64_t spin_secs;

static void out(int fd, const char* s, size_t n) {
    while (n) {
        int r = sys_write(fd, s, (uint32_t)n);
        if (r <= 0) return;
        s += r;
        n -= (size_t)r;
    }
}
static void outz(int fd, const char* s) { out(fd, s, strlen(s)); }

// Take the typed line or the waiting line off the screen, so output can go where it was.
static void clear_line(void) {
    if (ed_shown || spin_shown) outz(STDERR_FILENO, "\r\033[K");
    ed_shown = spin_shown = 0;
}

static void ed_draw(void) {
    if (!interactive || awaiting || !stdin_open || done) return;
    if (agent_busy(agent) && edn == 0) return;   // no prompt while it works, unless you are typing
    if (ed_shown) return;
    if (spin_shown) clear_line();
    outz(STDERR_FILENO, "> ");
    out(STDERR_FILENO, ed, (size_t)edn);
    ed_shown = 1;
}

static void spin_draw(void) {
    if (!http_id || !tty_out || awaiting || ed_shown) return;
    int64_t secs = (ops_now_ms() - http_since) / 1000;
    if (secs < 2 || (spin_shown && secs == spin_secs)) return;
    char b[96];
    snprintf(b, sizeof b, "\r\033[K\033[90m  waiting for the model, %llds (Ctrl-C stops it)\033[0m", (long long)secs);
    outz(STDERR_FILENO, b);
    spin_shown = 1;
    spin_secs = secs;
}

static const char* color(int style) {
    if (!tty_out) return "";
    switch (style) {
    case SAY_TOOL: return "\033[36m";
    case SAY_INFO: return "\033[90m";
    case SAY_ERROR: return "\033[31m";
    }
    return "";
}

static void show(int style, str_t s) {
    clear_line();
    if (style == SAY_ASSISTANT) {
        out(STDOUT_FILENO, s.p, s.n);
        outz(STDOUT_FILENO, "\n");
        return;
    }
    const char* on = color(style);
    const char* off = *on ? "\033[0m" : "";
    outz(STDERR_FILENO, on);
    if (style == SAY_TOOL && s.n && s.p[0] != ' ') outz(STDERR_FILENO, "> ");   // a tool's name line
    out(STDERR_FILENO, s.p, s.n);
    outz(STDERR_FILENO, off);
    outz(STDERR_FILENO, "\n");
}

static void ask(const act_t* a) {
    if (oneshot || !interactive) {
        clear_line();
        char b[200];
        snprintf(b, sizeof b, "%s(not allowed: there is no one to ask. Run with --yes to let barb write files and run commands unasked.)%s\n",
                 color(SAY_ERROR), tty_out ? "\033[0m" : "");
        outz(STDERR_FILENO, b);
        deliver_simple(EV_ANSWER, a->id, ANS_NO, 0);
        return;
    }
    clear_line();
    outz(STDERR_FILENO, "\n");
    outz(STDERR_FILENO, color(SAY_TOOL));
    out(STDERR_FILENO, a->a.p, a->a.n);
    outz(STDERR_FILENO, " wants to:");
    outz(STDERR_FILENO, tty_out ? "\033[0m\n" : "\n");
    out(STDERR_FILENO, a->b.p, a->b.n);
    outz(STDERR_FILENO, "\nAllow? [y]es / [n]o / [a]lways this session: ");
    awaiting = a->id;
}

// ---- carrying out actions ----

static void exec(const act_t* a) {
    switch (a->kind) {
    case ACT_SAY: show(a->flag, a->a); break;
    case ACT_HTTP: ops_http(a->id, a->a, a->b); http_id = a->id; http_since = ops_now_ms(); break;
    case ACT_READ: ops_read(a->id, a->a); break;
    case ACT_WRITE: ops_write(a->id, a->a, a->b); break;
    case ACT_SPAWN: ops_spawn(a->id, a->a, a->n, a->flag, a->c); break;
    case ACT_TIMER: ops_timer(a->id, a->n); break;
    case ACT_CANCEL:
        if (a->id == http_id) { http_id = 0; clear_line(); }
        if (a->id == awaiting) { awaiting = 0; outz(STDERR_FILENO, "\n"); }
        else ops_cancel(a->id);
        break;
    case ACT_ASK: ask(a); break;
    case ACT_DONE: done = 1; exit_code = (int)a->n; break;
    }
}

static void step(const ev_t* ev) {
    if (ev->kind == EV_HTTP && ev->id == http_id) { http_id = 0; if (spin_shown) clear_line(); }
    size_t n;
    const act_t* acts = agent_step(agent, ev, &n);
    for (size_t i = 0; i < n; i++) exec(&acts[i]);
}

static void drain_queue(void) {
    while (qhead && !done) {
        qev_t* q = qhead;
        qhead = q->next;
        if (!qhead) qtail = 0;
        step(&q->ev);
        free(q->data);
        free(q->note);
        free(q);
    }
}

// ---- the keyboard ----

static int esc_state;   // inside an escape sequence (arrow keys and the like), which are ignored

static void answer(int ans, const char* word) {
    int64_t id = awaiting;
    awaiting = 0;
    outz(STDERR_FILENO, word);
    outz(STDERR_FILENO, "\n");
    deliver_simple(EV_ANSWER, id, ans, 0);
}

static void key(unsigned char c) {
    if (esc_state) {   // ESC [ ... final, or ESC O x
        if (esc_state == 1) esc_state = (c == '[' || c == 'O') ? 2 : 0;
        else if (c >= 0x40 && c <= 0x7E) esc_state = 0;
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }
    if (awaiting) {   // one key answers; the line being typed waits
        switch (c) {
        case 'y': case 'Y': answer(ANS_YES, "yes"); break;
        case 'a': case 'A': answer(ANS_ALWAYS, "always"); break;
        case 'n': case 'N': case '\r': case '\n': case 4: answer(ANS_NO, "no"); break;
        case 3: deliver_simple(EV_INTERRUPT, 0, 0, 0); break;
        }
        return;
    }
    switch (c) {
    case 3:   // Ctrl-C: stop the work; at an idle prompt, throw away what was typed first
        if (!agent_busy(agent) && edn) {
            clear_line();
            edn = 0;
        } else {
            deliver_simple(EV_INTERRUPT, 0, 0, 0);
        }
        return;
    case 4:   // Ctrl-D on an empty line: the end
        if (edn) return;
        clear_line();
        outz(STDERR_FILENO, "\n");
        stdin_open = 0;
        deliver_simple(EV_EOF, 0, 0, 0);
        return;
    case '\r': case '\n': {
        ed_draw();
        outz(STDERR_FILENO, "\n");
        ed_shown = 0;
        ed[edn] = 0;
        deliver_simple(EV_INPUT, 0, 0, ed);
        edn = 0;
        return;
    }
    case 0x7F: case 0x08:   // Backspace: a whole UTF-8 character
        if (!edn) return;
        while (edn > 0 && ((unsigned char)ed[edn - 1] & 0xC0) == 0x80) edn--;
        if (edn > 0) edn--;
        clear_line();
        ed_draw();
        return;
    case 0x15:   // Ctrl-U: the whole line
        edn = 0;
        clear_line();
        ed_draw();
        return;
    case '\t':
        c = ' ';
        break;
    }
    if (c < 0x20 || edn >= (int)sizeof ed - 1) return;
    ed[edn++] = (char)c;
    if (ed_shown) out(STDERR_FILENO, (const char*)&c, 1);
    else ed_draw();
}

static unsigned char last_key;

static void read_keys(void) {
    unsigned char b[256];
    int r = sys_read(STDIN_FILENO, b, sizeof b);
    if (r <= 0) {
        if (r == -EINTR) return;
        stdin_open = 0;
        deliver_simple(EV_EOF, 0, 0, 0);
        return;
    }
    for (int i = 0; i < r; i++) {
        if (b[i] == '\n' && last_key == '\r') { last_key = 0; continue; }   // CR LF is one Enter
        last_key = b[i];
        key(b[i]);
    }
}

// Standard input that is not the terminal (a pipe, a file): lines, as they come.
static char inbuf[65536];
static uint32_t inlen;

static void read_lines(void) {
    int r = sys_read(STDIN_FILENO, inbuf + inlen, sizeof inbuf - inlen - 1);
    if (r == -EINTR) return;
    if (r <= 0) {
        stdin_open = 0;
        if (inlen) { inbuf[inlen] = 0; deliver_simple(EV_INPUT, 0, 0, inbuf); inlen = 0; }
        deliver_simple(EV_EOF, 0, 0, 0);
        return;
    }
    inlen += (uint32_t)r;
    uint32_t start = 0;
    for (uint32_t i = 0; i < inlen; i++) {
        if (inbuf[i] != '\n') continue;
        uint32_t e = i;
        if (e > start && inbuf[e - 1] == '\r') e--;
        inbuf[e] = 0;
        deliver_simple(EV_INPUT, 0, 0, inbuf + start);
        start = i + 1;
    }
    if (start) { memmove(inbuf, inbuf + start, inlen - start); inlen -= start; }
    if (inlen >= sizeof inbuf - 1) { inbuf[inlen] = 0; deliver_simple(EV_INPUT, 0, 0, inbuf); inlen = 0; }
}

// ---- the loop ----

static void loop(void) {
    pollfd_t fds[1 + OPS_MAX_FDS];
    for (;;) {
        if (got_sigint) { got_sigint = 0; deliver_simple(EV_INTERRUPT, 0, 0, 0); }
        if (got_sigterm) { done = 1; exit_code = 143; }
        drain_queue();
        if (done) return;
        ed_draw();
        spin_draw();
        int n = 0, stdin_slot = -1;
        if (stdin_open && !oneshot) { stdin_slot = n; fds[n].fd = STDIN_FILENO; fds[n].events = POLLIN; fds[n].revents = 0; n++; }
        int nops = ops_fill(fds + n);
        int timeout = ops_timeout_ms();
        if (http_id && tty_out && (timeout < 0 || timeout > 250)) timeout = 250;   // keep the clock moving
        if (qhead) timeout = 0;
        int r = sys_pollfd(fds, n + nops, timeout);
        if (r > 0 && stdin_slot >= 0 && fds[stdin_slot].revents) {
            if (interactive) read_keys(); else read_lines();
        }
        ops_service(fds + n, r > 0 ? nops : 0);
    }
}

// ---- starting up ----

static void usage(int fd) {
    char b[1400];
    snprintf(b, sizeof b,
        "barb %s: a coding agent for the terminal\n\n"
        "usage: barb [options]\n"
        "       barb -p TASK [options]\n\n"
        "  -p TASK             do one task and exit (TASK of - reads it from standard input,\n"
        "                      for a task longer than the shell's line)\n"
        "  -m MODEL            the model (default %s)\n"
        "  -y, --yes           do not ask before writing files or running commands\n"
        "  -C DIR              work in DIR\n"
        "  --url URL           the API's address (default %s)\n"
        "  --ca FILE           trust the certificates in FILE instead of /roots.pem\n"
        "  --config FILE       read the settings from FILE instead of %s\n"
        "  --max-turns N       stop after N model calls without you typing (default 60)\n"
        "  --no-instructions   do not read AGENTS.md or CLAUDE.md from the working directory\n"
        "  -V, --version\n"
        "  -h, --help\n\n"
        "%s holds the settings, one a line: key = ..., and optionally url = ...,\n"
        "model = ... and ca = ... (# starts a comment).\n",
        VERSION, DEFAULT_MODEL, DEFAULT_URL, CONFIG, CONFIG);
    outz(fd, b);
}

static char cfg_key[1024], cfg_url[512], cfg_model[128], cfg_ca[256];

static void trim(char** s, char** e) {
    while (*s < *e && (**s == ' ' || **s == '\t')) (*s)++;
    while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t' || (*e)[-1] == '\r')) (*e)--;
}

// The settings file. 1 if it was there, 0 if not, -1 (and a message) if it is wrong.
static int read_config(const char* path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return fd == -ENOENT ? 0 : -1;
    static char text[8192];
    int n = 0, r;
    while (n < (int)sizeof text - 1 && (r = sys_read(fd, text + n, sizeof text - 1 - (uint32_t)n)) > 0) n += r;
    sys_close(fd);
    text[n] = 0;
    int line_no = 0;
    for (char* line = text; *line;) {
        char* nl = strchr(line, '\n');
        char* end = nl ? nl : line + strlen(line);
        line_no++;
        char* s = line;
        char* e = end;
        trim(&s, &e);
        if (s < e && *s != '#') {
            char* eq = s;
            while (eq < e && *eq != '=') eq++;
            char* ks = s;
            char* ke = eq;
            char* vs = eq < e ? eq + 1 : e;
            char* ve = e;
            trim(&ks, &ke);
            trim(&vs, &ve);
            size_t kl = (size_t)(ke - ks), vl = (size_t)(ve - vs);
            char* dst = 0;
            size_t cap = 0;
            if (kl == 3 && !memcmp(ks, "key", 3)) { dst = cfg_key; cap = sizeof cfg_key; }
            else if (kl == 3 && !memcmp(ks, "url", 3)) { dst = cfg_url; cap = sizeof cfg_url; }
            else if (kl == 5 && !memcmp(ks, "model", 5)) { dst = cfg_model; cap = sizeof cfg_model; }
            else if (kl == 2 && !memcmp(ks, "ca", 2)) { dst = cfg_ca; cap = sizeof cfg_ca; }
            if (eq == e || !dst || vl >= cap) {
                dprintf(STDERR_FILENO, "barb: %s, line %d: %s\n", path, line_no,
                        eq == e ? "expected name = value" : !dst ? "unknown setting (key, url, model, ca)" : "value too long");
                return -1;
            }
            memcpy(dst, vs, vl);
            dst[vl] = 0;
        }
        line = nl ? nl + 1 : end;
    }
    return 1;
}

int main(int argc, char** argv) {
    const char* prompt = 0;
    const char* model = 0;
    const char* url = 0;
    const char* ca = 0;
    const char* cwd_arg = 0;
    const char* config = CONFIG;
    int yes = 0, max_turns = 0, no_instructions = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        int has_val = i + 1 < argc;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(STDOUT_FILENO); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("barb %s (RingTail)\n", VERSION); return 0; }
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes")) yes = 1;
        else if (!strcmp(a, "--no-instructions")) no_instructions = 1;
        else if (!strcmp(a, "-p") && has_val) prompt = argv[++i];
        else if (!strcmp(a, "-m") && has_val) model = argv[++i];
        else if (!strcmp(a, "-C") && has_val) cwd_arg = argv[++i];
        else if (!strcmp(a, "--url") && has_val) url = argv[++i];
        else if (!strcmp(a, "--ca") && has_val) ca = argv[++i];
        else if (!strcmp(a, "--config") && has_val) config = argv[++i];
        else if (!strcmp(a, "--max-turns") && has_val) {
            uint32_t v;
            if (parse_uint(argv[++i], &v) || v == 0 || v > 100000) { usage(STDERR_FILENO); return 2; }
            max_turns = (int)v;
        } else { usage(STDERR_FILENO); return 2; }
    }

    int c = read_config(config);
    if (c < 0) {
        if (c != -1) dprintf(STDERR_FILENO, "barb: %s: %s\n", config, strerror(c));
        return 2;
    }
    if (!cfg_key[0]) {
        dprintf(STDERR_FILENO, "barb: no API key. Put a line \"key = ...\" in %s%s.\n", config,
                c ? "" : " (it does not exist yet)");
        return 2;
    }
    if (!model) model = cfg_model[0] ? cfg_model : DEFAULT_MODEL;
    if (!url) url = cfg_url[0] ? cfg_url : DEFAULT_URL;
    if (!ca && cfg_ca[0]) ca = cfg_ca;
    static char base[512];
    snprintf(base, sizeof base, "%s", url);
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    if (!strncmp(base, "http://", 7) && strncmp(base + 7, "127.0.0.1", 9) && strncmp(base + 7, "localhost", 9))
        dprintf(STDERR_FILENO, "barb: warning: %s is plain http, so your API key will cross the network unencrypted.\n", base);

    if (cwd_arg) {
        int r = sys_chdir(cwd_arg);
        if (r < 0) { dprintf(STDERR_FILENO, "barb: cannot change to %s: %s\n", cwd_arg, strerror(r)); return 2; }
    }
    static char cwd[PATH_MAX];
    if (sys_getcwd(cwd, sizeof cwd) < 0) snprintf(cwd, sizeof cwd, "/");
    static char date[16];
    rtc_time_t t;
    sys_time(&t);
    snprintf(date, sizeof date, "%04u-%02u-%02u", t.year, t.month, t.day);
    static char plat[640];
    char ver[120] = "";
    sys_kinfo(KINFO_VERSION, ver, sizeof ver);
    for (char* p = ver; *p; p++) if (*p == '\n') *p = 0;
    // What the model would otherwise spend its first calls finding out.
    snprintf(plat, sizeof plat,
             "RingTail (%s), a small hobby OS, not Unix. bash runs saklas, a POSIX-like sh with pipes, "
             "redirection, $(( )) and while/if, but no job control. The programs are files in / (ls / lists "
             "them): ls cat cp mv rm mkdir grep sed sort uniq head tail wc diff tr cut find xargs printf seq "
             "sleep echo fetch and more. There is no compiler, no Python and no package manager. File names "
             "are FAT long names: ASCII, case-insensitive.",
             ver[0] ? ver : "RingTail OS");

    static char task[65536];
    if (prompt && !strcmp(prompt, "-")) {
        int n = 0, r;
        while (n < (int)sizeof task - 1 && (r = sys_read(STDIN_FILENO, task + n, sizeof task - 1 - (uint32_t)n)) > 0) n += r;
        task[n] = 0;
        prompt = task;
    }
    oneshot = prompt != 0;
    interactive = !oneshot && sys_fdtype(STDIN_FILENO) == FDTYPE_TTY;
    tty_out = sys_fdtype(STDERR_FILENO) == FDTYPE_TTY;
    if (interactive) {
        old_mode = sys_tty_mode(TTY_RAW | TTY_NOSIG);
        if (old_mode < 0) interactive = 0;   // not in the foreground: read lines instead
    }
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    agent_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.model = str_z(model);
    cfg.cwd = str_z(cwd);
    cfg.platform = str_z(plat);
    cfg.date = str_z(date);
    cfg.auto_approve = yes;
    cfg.max_turns = max_turns;
    cfg.instructions = !no_instructions;
    agent = agent_new(&mem, &cfg);
    if (!agent) { outz(STDERR_FILENO, "barb: out of memory\n"); return 1; }
    ops_init(deliver, base, cfg_key, ca);
    memset(cfg_key, 0, sizeof cfg_key);   // ops keeps its own copy

    if (!oneshot) {
        char b[700];
        snprintf(b, sizeof b, "barb %s, model %s, in %s\n/help for commands; Ctrl-D to exit.\n", VERSION, model, cwd);
        outz(STDERR_FILENO, b);
    } else {
        deliver_simple(EV_INPUT, 0, 0, prompt);
        deliver_simple(EV_EOF, 0, 0, 0);
    }
    loop();
    clear_line();
    if (old_mode >= 0) sys_tty_mode(old_mode);
    ops_shutdown();
    agent_free(agent);
    return exit_code;
}

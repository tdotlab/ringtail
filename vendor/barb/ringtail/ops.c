// What the agent's actions turn into on RingTail: programs, HTTP through fetch, files, timers.
//
// RingTail has no fork, no environment and nothing like SIGCHLD, so this is shaped differently
// from src/posix/ops.c while doing the same job:
//
// - A command is `sh -c CMD`, spawned as a job of its own (SPAWN_NEW_JOB), so kill(-job) stops
//   it together with whatever it started. Its standard input is an empty pipe, never the
//   terminal. Its end is noticed by asking kill(id, 0), which fails once the task is dead; join
//   then gives its exit status. A foreground command's output comes through a pipe; a background
//   task's goes straight into its log file.
// - HTTP is the fetch program, spawned the same way: the request body in a file, the headers on
//   its standard input. The API key is in those headers, never in fetch's arguments, which any
//   program can read in the task table. fetch -i prints the status line and headers before the
//   body, and -e keeps the body of an error response, which says what went wrong.
// - Files are read and written whole. A write up to 1 MB is one sys_save, which replaces the file
//   whole or not at all; a bigger one goes through a temporary file and a rename.
// - The clock is the kernel's tick counter (100 Hz).
#include "ops.h"

#define MAX_OPS 32
#define READ_LIMIT (4u << 20)     // the most read_file takes; memory is tighter here than on Linux
#define HTTP_LIMIT (32u << 20)    // the most a reply may be
#define HEAD_KEEP (64u << 10)     // of a command's output: the start...
#define TAIL_KEEP (64u << 10)     // ...and the end; the middle is dropped, and the model told
#define GRACE_MS 2000             // between SIGTERM and SIGKILL
#define SAVE_MAX (1u << 20)       // sys_save's limit
#define FETCH_IDLE "900"          // seconds fetch waits for a quiet server: a model may think a while

enum { K_FREE, K_PROC, K_HTTP, K_TIMER };

typedef struct { char* p; uint32_t n, cap; } buf_t;

typedef struct {
    int kind;
    int64_t id;
    int task;                 // the program's task id, which is also its job's; 0 if none
    int fd[2];                // pipes being read: output, and fetch's messages; -1 once closed
    int bg, exited, status, cancelled, timed_out, too_big;
    int64_t started, deadline, kill_at, exited_at;
    buf_t head, errs;         // the start of the output (all of a reply); fetch's messages
    char* ring;               // the most recent output past the start
    uint64_t ring_pos, total;
    char req[40];             // the request body's file, for HTTP
} op_t;

static op_t ops[MAX_OPS];
static deliver_fn deliver;
static char g_url[512], g_key[1024], g_ca[256];
static int bg_jobs[64], nbg;  // background jobs, stopped when barb exits

int64_t ops_now_ms(void) { return (int64_t)sys_ticks() * 10; }

void ops_init(deliver_fn d, const char* base_url, const char* api_key, const char* ca_file) {
    deliver = d;
    snprintf(g_url, sizeof g_url, "%s", base_url);
    snprintf(g_key, sizeof g_key, "%s", api_key);
    snprintf(g_ca, sizeof g_ca, "%s", ca_file ? ca_file : "");
}

// ---- small things ----

static void buf_add(buf_t* b, const char* p, uint32_t n) {
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char* q = realloc(b->p, cap);
        if (!q) return;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static op_t* new_op(int kind, int64_t id) {
    for (int i = 0; i < MAX_OPS; i++) {
        if (ops[i].kind != K_FREE) continue;
        memset(&ops[i], 0, sizeof ops[i]);
        ops[i].kind = kind;
        ops[i].id = id;
        ops[i].fd[0] = ops[i].fd[1] = -1;
        ops[i].started = ops_now_ms();
        return &ops[i];
    }
    return 0;
}

static void release(op_t* o) {
    for (int k = 0; k < 2; k++) if (o->fd[k] >= 0) sys_close(o->fd[k]);
    if (o->req[0]) sys_delete(o->req);
    free(o->head.p);
    free(o->errs.p);
    free(o->ring);
    memset(o, 0, sizeof *o);
    o->kind = K_FREE;
}

static void fail_start(int64_t id, int kind, const char* why) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.note = str_z(why);
    if (kind == K_HTTP) { e.kind = EV_HTTP; e.status = 0; }
    else { e.kind = EV_PROC; e.flags = PROC_FAILED; e.status = -1; }
    deliver(&e);
}

// All the directories above `path`. Errors are left for the open or save that follows to report.
static void mkdir_p(const char* path) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof copy, "%s", path);
    for (char* p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        sys_mkdir(copy);
        *p = '/';
    }
}

// A program as the leader of a job of its own, with the descriptors given as its 0, 1 and 2.
static int spawn_job(const char* prog, const char* const* argv, int in, int out, int err) {
    spawn_opts_t o;
    memset(&o, 0, sizeof o);
    o.flags = SPAWN_NEW_JOB;
    o.fd[0] = in;
    o.fd[1] = out;
    o.fd[2] = err;
    o.fd3 = -1;
    return sys_spawn_opts(prog, argv, &o);
}

// The read end of a pipe nobody writes to: standard input that is at its end at once.
static int empty_input(void) {
    int p[2];
    if (sys_pipe(p) < 0) return -1;
    sys_close(p[1]);
    return p[0];
}

static int write_all(int fd, const char* p, uint32_t n) {
    while (n) {
        int r = sys_write(fd, p, n);
        if (r <= 0) return r < 0 ? r : -EIO;
        p += r;
        n -= (uint32_t)r;
    }
    return 0;
}

// ---- programs ----

// Keep the start of a command's output, then a ring of the most recent bytes.
static void proc_add(op_t* o, const char* p, uint32_t n) {
    o->total += n;
    if (o->head.n < HEAD_KEEP) {
        uint32_t take = HEAD_KEEP - o->head.n;
        if (take > n) take = n;
        buf_add(&o->head, p, take);
        p += take;
        n -= take;
    }
    if (!n) return;
    if (!o->ring && !(o->ring = malloc(TAIL_KEEP))) return;
    for (uint32_t i = 0; i < n; i++) o->ring[(o->ring_pos + i) % TAIL_KEEP] = p[i];
    o->ring_pos += n;
}

// The output as one string: the start, a note if the middle was dropped, the end.
static char* proc_output(op_t* o, uint32_t* len) {
    uint32_t head = o->head.n;
    uint64_t rest = o->total - head;
    uint32_t tail = rest > TAIL_KEEP ? TAIL_KEEP : (uint32_t)rest;
    uint64_t dropped = rest - tail;
    char note[80] = "";
    if (dropped) snprintf(note, sizeof note, "\n[... %u bytes of output dropped here ...]\n", (unsigned)dropped);
    uint32_t nl = (uint32_t)strlen(note);
    char* out = malloc(head + nl + tail + 1);
    if (!out) { *len = 0; return 0; }
    uint32_t n = 0;
    if (head) { memcpy(out, o->head.p, head); n = head; }
    memcpy(out + n, note, nl);
    n += nl;
    for (uint32_t i = 0; i < tail; i++) out[n + i] = o->ring[(o->ring_pos - tail + i) % TAIL_KEEP];
    n += tail;
    out[n] = 0;
    *len = n;
    return out;
}

void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log) {
    op_t* o = new_op(K_PROC, id);
    if (!o) { fail_start(id, K_PROC, "too many operations at once"); return; }
    int in = empty_input(), out = -1, rd = -1;
    char why[160] = "";
    if (bg) {
        mkdir_p(log.p);
        out = sys_open(log.p, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) snprintf(why, sizeof why, "cannot open the log file %s: %s", log.p, strerror(out));
    } else {
        int p[2];
        int r = sys_pipe(p);
        if (r < 0) snprintf(why, sizeof why, "cannot make a pipe: %s", strerror(r));
        else { rd = p[0]; out = p[1]; }
    }
    int task = -1;
    if (!why[0] && in >= 0) {
        const char* argv[] = { "sh", "-c", cmd.p, 0 };
        task = spawn_job("sh", argv, in, out, out);
        if (task < 0) snprintf(why, sizeof why, "cannot start sh: %s", strerror(task));
    } else if (!why[0]) {
        snprintf(why, sizeof why, "cannot make a pipe");
    }
    if (in >= 0) sys_close(in);
    if (out >= 0) sys_close(out);
    if (why[0]) {
        if (rd >= 0) sys_close(rd);
        release(o);
        fail_start(id, K_PROC, why);
        return;
    }
    o->task = task;
    o->fd[0] = rd;
    o->bg = bg;
    if (!bg && timeout_ms > 0) o->deadline = o->started + timeout_ms;
    if (bg && nbg < (int)(sizeof bg_jobs / sizeof bg_jobs[0])) bg_jobs[nbg++] = task;
}

// ---- HTTP ----

void ops_http(int64_t id, str_t path, str_t body) {
    op_t* o = new_op(K_HTTP, id);
    if (!o) { fail_start(id, K_HTTP, "too many operations at once"); return; }
    // The body, in a file of this request's own, so a cancelled fetch still starting up can
    // never read the next request's.
    snprintf(o->req, sizeof o->req, "/barb-%lld.req", (long long)id);
    int fd = sys_open(o->req, O_WRONLY | O_CREAT | O_TRUNC);
    int r = fd < 0 ? fd : write_all(fd, body.p, (uint32_t)body.n);
    if (fd >= 0) sys_close(fd);
    char why[200];
    if (r < 0) {
        snprintf(why, sizeof why, "cannot write the request to %s: %s", o->req, strerror(r));
        release(o);
        fail_start(id, K_HTTP, why);
        return;
    }
    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
    if (sys_pipe(in) < 0 || sys_pipe(out) < 0 || sys_pipe(err) < 0) {
        for (int k = 0; k < 2; k++) {
            if (in[k] >= 0) sys_close(in[k]);
            if (out[k] >= 0) sys_close(out[k]);
            if (err[k] >= 0) sys_close(err[k]);
        }
        release(o);
        fail_start(id, K_HTTP, "cannot make a pipe for fetch");
        return;
    }
    // The headers, the key among them, go through the pipe; they fit in its 4 KB, so this
    // write does not wait for fetch.
    char h[sizeof g_key + 160];
    int hn = snprintf(h, sizeof h, "x-api-key: %s\nanthropic-version: 2023-06-01\ncontent-type: application/json\n", g_key);
    write_all(in[1], h, (uint32_t)hn);
    memset(h, 0, sizeof h);
    sys_close(in[1]);
    char url[sizeof g_url + 256];
    snprintf(url, sizeof url, "%s%s", g_url, path.p);   // RingTail's printf has no precision; path ends with a NUL
    const char* argv[16];
    int a = 0;
    argv[a++] = "fetch"; argv[a++] = "-i"; argv[a++] = "-e";
    argv[a++] = "-d"; argv[a++] = o->req;
    argv[a++] = "-H"; argv[a++] = "-";
    argv[a++] = "-t"; argv[a++] = FETCH_IDLE;
    if (g_ca[0]) { argv[a++] = "--ca"; argv[a++] = g_ca; }
    argv[a++] = url;
    argv[a] = 0;
    int task = spawn_job("fetch", argv, in[0], out[1], err[1]);
    sys_close(in[0]);
    sys_close(out[1]);
    sys_close(err[1]);
    if (task < 0) {
        sys_close(out[0]);
        sys_close(err[0]);
        snprintf(why, sizeof why, "cannot start fetch: %s", strerror(task));
        release(o);
        fail_start(id, K_HTTP, why);
        return;
    }
    o->task = task;
    o->fd[0] = out[0];
    o->fd[1] = err[0];
}

// fetch -i printed the status line and the headers, then the body. 0 if there is no status line.
static int http_status(const char* p, uint32_t n, uint32_t* body_at) {
    uint32_t at = 0;
    int status = 0;
    while (n - at >= 12 && !memcmp(p + at, "HTTP/1.", 7)) {
        status = 0;
        for (uint32_t i = at + 9; i < at + 12 && p[i] >= '0' && p[i] <= '9'; i++) status = status * 10 + (p[i] - '0');
        uint32_t end = at;
        while (end + 1 < n && !(p[end] == '\n' && (p[end + 1] == '\n' || (p[end + 1] == '\r' && end + 2 < n && p[end + 2] == '\n')))) end++;
        if (end + 1 >= n) return 0;   // the headers never ended
        at = end + (p[end + 1] == '\n' ? 2 : 3);
        if (status < 100 || status > 199 || status == 101) break;   // a 1xx is an aside; the real one follows
    }
    *body_at = at;
    return status;
}

// ---- files ----

static int fs_status(int r) {
    switch (-r) {
    case ENOENT: return FS_NOTFOUND;
    case EISDIR: return FS_ISDIR;
    case EFBIG: return FS_TOOBIG;
    case EPERM: return FS_DENIED;
    default: return FS_ERROR;
    }
}

static void file_event(int64_t id, int status, const char* note, const char* data, uint32_t n) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.kind = EV_FILE;
    e.id = id;
    e.status = status;
    if (note) e.note = str_z(note);
    if (data) e.data = str_of(data, n);
    deliver(&e);
}

void ops_read(int64_t id, str_t path) {
    dirent_t d;
    int r = sys_stat(path.p, &d);
    if (r < 0) { file_event(id, fs_status(r), strerror(r), 0, 0); return; }
    if (d.flags & DIRENT_DIR) { file_event(id, FS_ISDIR, 0, 0, 0); return; }
    if (d.size > READ_LIMIT) {
        char note[96];
        snprintf(note, sizeof note, "%u bytes, the limit is %u", d.size, READ_LIMIT);
        file_event(id, FS_TOOBIG, note, 0, 0);
        return;
    }
    int fd = sys_open(path.p, O_RDONLY);
    if (fd < 0) { file_event(id, fs_status(fd), strerror(fd), 0, 0); return; }
    buf_t b = { 0, 0, 0 };
    char chunk[4096];
    while ((r = sys_read(fd, chunk, sizeof chunk)) > 0) {
        buf_add(&b, chunk, (uint32_t)r);
        if (b.n > READ_LIMIT) break;
    }
    sys_close(fd);
    if (b.n > READ_LIMIT) file_event(id, FS_TOOBIG, "more than the size limit", 0, 0);
    else if (r < 0) file_event(id, fs_status(r), strerror(r), 0, 0);
    else file_event(id, FS_OK, 0, b.p ? b.p : "", b.n);
    free(b.p);
}

void ops_write(int64_t id, str_t path, str_t data) {
    mkdir_p(path.p);
    int r;
    if (data.n <= SAVE_MAX) {
        r = sys_save(path.p, data.p, (uint32_t)data.n);   // whole or not at all
    } else {
        // Too big for one save: a new file beside it, then swapped in. Not atomic, but the old
        // contents stay intact until the new ones are all on the disk.
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s.barb-new", path.p);
        int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
        r = fd < 0 ? fd : write_all(fd, data.p, (uint32_t)data.n);
        if (fd >= 0) sys_close(fd);
        if (r == 0) {
            sys_delete(path.p);
            r = sys_rename(tmp, path.p);
        } else {
            sys_delete(tmp);
        }
    }
    if (r < 0) file_event(id, fs_status(r), strerror(r), 0, 0);
    else file_event(id, FS_OK, 0, 0, 0);
}

// ---- timers and cancelling ----

void ops_timer(int64_t id, int64_t ms) {
    op_t* o = new_op(K_TIMER, id);
    if (!o) return;   // the agent's timer never fires; it cannot be told otherwise
    o->deadline = o->started + ms;
}

static op_t* find(int64_t id) {
    for (int i = 0; i < MAX_OPS; i++) if (ops[i].kind != K_FREE && ops[i].id == id) return &ops[i];
    return 0;
}

void ops_cancel(int64_t id) {
    op_t* o = find(id);
    if (!o) return;
    if (o->kind == K_TIMER) { release(o); return; }
    o->cancelled = 1;
    if (!o->exited && o->task > 0) {
        // fetch has nothing to tidy: kill it outright. A command gets the chance to.
        sys_kill(-o->task, o->kind == K_HTTP ? SIGKILL : SIGTERM);
        o->kill_at = ops_now_ms() + GRACE_MS;
    }
}

// ---- the loop's side ----

static int map_op[OPS_MAX_FDS], map_k[OPS_MAX_FDS];

int ops_fill(pollfd_t* fds) {
    int n = 0;
    for (int i = 0; i < MAX_OPS && n < OPS_MAX_FDS; i++)
        for (int k = 0; k < 2 && n < OPS_MAX_FDS; k++) {
            if (ops[i].kind == K_FREE || ops[i].fd[k] < 0) continue;
            fds[n].fd = ops[i].fd[k];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            map_op[n] = i;
            map_k[n] = k;
            n++;
        }
    return n;
}

int ops_timeout_ms(void) {
    int64_t now = ops_now_ms(), best = -1;
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        int64_t t = -1;
        if (o->kind == K_TIMER) t = o->deadline - now;
        else if (!o->exited) {
            // Nothing tells us a task has ended, so look: often for what someone waits on,
            // now and then for a background task.
            t = o->bg ? 250 : 50;
            if (o->deadline && o->deadline - now < t) t = o->deadline - now;
            if (o->kill_at && o->kill_at - now < t) t = o->kill_at - now;
        } else {
            t = 50;   // ended; its pipes are being emptied
        }
        if (t < 0) t = 0;
        if (best < 0 || t < best) best = t;
    }
    return best > 0x7FFFFFFF ? 0x7FFFFFFF : (int)best;
}

static void finish(op_t* o) {
    ev_t e;
    memset(&e, 0, sizeof e);
    e.id = o->id;
    e.ms = ops_now_ms() - o->started;
    int sig = EXIT_SIGNUM(o->status);
    int code = sig || o->status < 0 ? -1 : o->status;
    if (o->kind == K_PROC) {
        if (!o->cancelled || o->bg) {   // an interrupted foreground command has no listener
            uint32_t n = 0;
            char* out = o->bg ? 0 : proc_output(o, &n);
            e.kind = EV_PROC;
            e.status = code;
            e.flags = (o->timed_out ? PROC_TIMEDOUT : 0) | (sig || o->cancelled ? PROC_SIGNALED : 0);
            if (out) e.data = str_of(out, n);
            deliver(&e);
            free(out);
        }
    } else if (!o->cancelled) {
        e.kind = EV_HTTP;
        uint32_t body_at = 0;
        int status = o->too_big ? 0 : http_status(o->head.p ? o->head.p : "", o->head.n, &body_at);
        // A reply that did not arrive whole (fetch failed after the status line) is a failed
        // request, which the agent retries, not a reply.
        if (status && (code == 0 || status >= 400)) {
            e.status = status;
            e.data = str_of(o->head.p + body_at, o->head.n - body_at);
        } else {
            char note[400];
            uint32_t n = o->errs.n;
            const char* m = o->errs.p ? o->errs.p : "";
            while (n && (m[n - 1] == '\n' || m[n - 1] == '\r' || m[n - 1] == ' ')) n--;
            if (o->too_big) snprintf(note, sizeof note, "the reply is bigger than %u bytes", HTTP_LIMIT);
            else if (n) { if (n > 300) n = 300; memcpy(note, m, n); note[n] = 0; }
            else if (sig) snprintf(note, sizeof note, "fetch was stopped by signal %d", sig);
            else snprintf(note, sizeof note, "fetch failed with exit status %d", o->status);
            e.status = 0;
            e.note = str_z(note);
        }
        deliver(&e);
    }
    release(o);
}

void ops_service(const pollfd_t* fds, int n) {
    char chunk[16384];
    for (int i = 0; i < n; i++) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
        op_t* o = &ops[map_op[i]];
        int k = map_k[i];
        if (o->kind == K_FREE || o->fd[k] != fds[i].fd) continue;
        int r = sys_read(o->fd[k], chunk, sizeof chunk);
        if (r <= 0) {
            sys_close(o->fd[k]);
            o->fd[k] = -1;
            continue;
        }
        if (o->kind == K_PROC) proc_add(o, chunk, (uint32_t)r);
        else if (k == 1) { if (o->errs.n < 4096) buf_add(&o->errs, chunk, (uint32_t)r); }
        else if (o->head.n + (uint32_t)r > HTTP_LIMIT) o->too_big = 1;
        else buf_add(&o->head, chunk, (uint32_t)r);
    }

    int64_t now = ops_now_ms();
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        if (o->kind == K_TIMER) {
            if (now >= o->deadline) {
                ev_t e;
                memset(&e, 0, sizeof e);
                e.kind = EV_TIMER;
                e.id = o->id;
                release(o);
                deliver(&e);
            }
            continue;
        }
        if (!o->exited && sys_kill(o->task, 0) == -ESRCH) {
            o->exited = 1;
            o->exited_at = now;
            o->status = sys_join(o->task);
            // What it left running would hold the output pipe open; a background task's
            // leftovers are left alone (they are stopped when barb exits).
            if (!o->bg) sys_kill(-o->task, SIGKILL);
        }
        if (!o->exited) {
            if (o->deadline && now >= o->deadline && !o->timed_out) {
                o->timed_out = 1;
                sys_kill(-o->task, SIGTERM);
                o->kill_at = now + GRACE_MS;
            }
            if (o->kill_at && now >= o->kill_at) {
                sys_kill(-o->task, SIGKILL);
                o->kill_at = 0;
            }
            continue;
        }
        // Ended: take what is still in the pipes, for at most a second.
        if ((o->fd[0] >= 0 || o->fd[1] >= 0) && now - o->exited_at < 1000) continue;
        finish(o);
    }
}

void ops_shutdown(void) {
    for (int i = 0; i < MAX_OPS; i++) {
        op_t* o = &ops[i];
        if (o->kind == K_FREE) continue;
        if ((o->kind == K_PROC || o->kind == K_HTTP) && !o->exited && o->task > 0) sys_kill(-o->task, SIGKILL);
        release(o);
    }
    for (int i = 0; i < nbg; i++) sys_kill(-bg_jobs[i], SIGTERM);
    nbg = 0;
}

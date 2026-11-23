// The tools the model can call, and their definitions.
//
// These are written for a model to read. A tool's result says what happened
// in plain words, and an error says what to do next ("read the file first",
// "add more context so old_string is unique"), not just that something went
// wrong, so the model can correct itself in one step. Output that could be
// huge is cut in the middle, where the least is usually lost, and says so.
//
// A tool is a small state machine because the core does no I/O: it asks the
// platform to read a file and returns; the answer arrives as an event and
// the tool continues. tc.phase is where it is; tool_begin starts it and
// tool_event continues it. Everything a tool keeps between steps lives in
// agent->ta (emptied when the call ends), because the step's own scratch
// memory is gone by the next event.
#include "agent_int.h"
#include <string.h>

#define J(...) #__VA_ARGS__

const char TOOLS_JSON[] = J([
  {"name":"read_file",
   "description":"Read a text file. Returns the lines, each prefixed with its line number and a tab. Paths are relative to the working directory, or absolute. By default up to 2000 lines are returned; use offset and limit to read further into a large file.",
   "input_schema":{"type":"object","properties":{
     "path":{"type":"string","description":"The file to read."},
     "offset":{"type":"integer","description":"The line to start at, counting from 1."},
     "limit":{"type":"integer","description":"The most lines to return."}},
     "required":["path"]}},
  {"name":"write_file",
   "description":"Create a file, or replace one completely. Parent directories are created. If the file already exists you must have read it first in this session. To change part of an existing file, use edit_file instead.",
   "input_schema":{"type":"object","properties":{
     "path":{"type":"string","description":"The file to write."},
     "content":{"type":"string","description":"The whole new contents of the file."}},
     "required":["path","content"]}},
  {"name":"edit_file",
   "description":"Replace an exact piece of text in an existing file. old_string must match the file exactly, including whitespace, indentation and line endings, and must occur once unless replace_all is true: include a few neighbouring lines to make it unique. You must have read the file first in this session.",
   "input_schema":{"type":"object","properties":{
     "path":{"type":"string","description":"The file to edit."},
     "old_string":{"type":"string","description":"The exact text to replace."},
     "new_string":{"type":"string","description":"The text to put in its place. May be empty to delete."},
     "replace_all":{"type":"boolean","description":"Replace every occurrence instead of requiring exactly one."}},
     "required":["path","old_string","new_string"]}},
  {"name":"bash",
   "description":"Run a shell command in the working directory. Standard output and standard error come back together, followed by the exit code. A command that fails (a non-zero exit code) is not an error in itself: read the output. Output longer than about 30000 characters is cut in the middle. For anything that may take more than a minute or two, set run_in_background: you get a task number at once, the output goes to a log file, and you are woken with a message when it exits.",
   "input_schema":{"type":"object","properties":{
     "command":{"type":"string","description":"The command line, run with sh -c."},
     "description":{"type":"string","description":"A few words on what the command does, shown to the user."},
     "timeout_ms":{"type":"integer","description":"How long to let it run before it is killed. Default 120000, at most 600000. Ignored for background tasks."},
     "run_in_background":{"type":"boolean","description":"Start it and return immediately; you will be woken when it exits."}},
     "required":["command"]}},
  {"name":"task_stop",
   "description":"Stop a background task started with bash and run_in_background.",
   "input_schema":{"type":"object","properties":{
     "task_id":{"type":"integer","description":"The task number you were given when it started."}},
     "required":["task_id"]}},
  {"name":"schedule_wakeup",
   "description":"Wake yourself after a delay. Barb sends you a message at that time. Nothing runs and no tokens are spent while you wait, so end your turn after calling this. Use it to check back on something later.",
   "input_schema":{"type":"object","properties":{
     "seconds":{"type":"integer","description":"How long to wait, from 1 to 86400."},
     "note":{"type":"string","description":"What you want to remember when you wake."}},
     "required":["seconds"]}}
]);

#define READ_DEFAULT_LINES 2000
#define READ_MAX_LINES 10000
#define READ_MAX_LINE_BYTES 2000
#define READ_MAX_BYTES 100000
#define OUTPUT_MAX 30000
#define BASH_DEFAULT_MS 120000
#define BASH_MAX_MS 600000

// ---- helpers ----

str_t cap_output(arena_t* a, str_t s, size_t max) {
    if (s.n <= max) return s;
    size_t head = max / 2, tail_from = s.n - max / 2;
    while (head > 0 && ((unsigned char)s.p[head] & 0xC0) == 0x80) head--;           // do not cut a character
    while (tail_from < s.n && ((unsigned char)s.p[tail_from] & 0xC0) == 0x80) tail_from++;
    sb_t b;
    sb_init(&b, a);
    sb_put(&b, s.p, head);
    sb_putz(&b, "\n\n... [");
    sb_putu(&b, (uint64_t)(tail_from - head));
    sb_putz(&b, " bytes omitted from the middle] ...\n\n");
    sb_put(&b, s.p + tail_from, s.n - tail_from);
    str_t r = sb_str(&b);
    return r.p ? r : s;
}

static void fail(agent_t* g, const char* msg) { finish_call(g, str_z(msg), 1, 0); }
static void fail_sb(agent_t* g, sb_t* b) { finish_call(g, sb_str(b), 1, 0); }

static str_t first_line(str_t s, size_t max) {
    size_t n = 0;
    while (n < s.n && s.p[n] != '\n' && n < max) n++;
    return str_of(s.p, n);
}

// The input must be a JSON object; then each argument is looked up by name.
static int input_ok(agent_t* g, call_t* c) {
    str_t in = str_trim(c->input);
    if (!json_valid(in) || in.n == 0 || in.p[0] != '{') {
        fail(g, "The tool input must be a JSON object with the arguments in the tool's schema.");
        return 0;
    }
    return 1;
}

static int arg_missing(agent_t* g, const char* key, const char* type) {
    sb_t b;
    sb_init(&b, &g->tmp);
    sb_putz(&b, "Missing required argument '");
    sb_putz(&b, key);
    sb_putz(&b, "' (");
    sb_putz(&b, type);
    sb_putz(&b, ").");
    fail_sb(g, &b);
    return 0;
}

static int arg_wrong(agent_t* g, const char* key, const char* type) {
    sb_t b;
    sb_init(&b, &g->tmp);
    sb_putz(&b, "Argument '");
    sb_putz(&b, key);
    sb_putz(&b, "' must be ");
    sb_putz(&b, type);
    sb_putz(&b, ".");
    fail_sb(g, &b);
    return 0;
}

// A string argument, copied into ta. An absent optional one gives {0,0}. 0 if the call was failed.
static int arg_str(agent_t* g, call_t* c, const char* key, int required, str_t* out) {
    str_t raw, s;
    *out = str_of(0, 0);
    if (!json_get(c->input, key, &raw)) return required ? arg_missing(g, key, "a string") : 1;
    if (!json_as_str(&g->ta, raw, &s)) return arg_wrong(g, key, "a string");
    *out = s;
    return 1;
}

static int arg_int(agent_t* g, call_t* c, const char* key, int required, int64_t* out, int* present) {
    str_t raw;
    *present = 0;
    if (!json_get(c->input, key, &raw)) return required ? arg_missing(g, key, "an integer") : 1;
    if (!json_as_int(raw, out)) return arg_wrong(g, key, "an integer");
    *present = 1;
    return 1;
}

static int arg_bool(agent_t* g, call_t* c, const char* key, int* out) {
    str_t raw;
    *out = 0;
    if (!json_get(c->input, key, &raw)) return 1;
    if (!json_as_bool(raw, out)) return arg_wrong(g, key, "true or false");
    return 1;
}

static int has_nul(str_t s) { return s.n && memchr(s.p, 0, s.n) != 0; }

// Drop "./", doubled slashes and a trailing slash, so "src//a.c" and "./src/a.c" are one file.
static str_t norm_path(arena_t* a, str_t p) {
    sb_t b;
    sb_init(&b, a);
    int abs = p.n && p.p[0] == '/';
    if (abs) sb_putc(&b, '/');
    size_t i = 0;
    int first = 1;
    while (i < p.n) {
        size_t j = i;
        while (j < p.n && p.p[j] != '/') j++;
        size_t len = j - i;
        if (len > 0 && !(len == 1 && p.p[i] == '.')) {
            if (!first) sb_putc(&b, '/');
            sb_put(&b, p.p + i, len);
            first = 0;
        }
        i = j + 1;
    }
    if (first && !abs) sb_putc(&b, '.');
    return sb_str(&b);
}

static int arg_path(agent_t* g, call_t* c, str_t* out) {
    str_t raw;
    if (!arg_str(g, c, "path", 1, &raw)) return 0;
    if (raw.n == 0) return arg_wrong(g, "path", "a non-empty file path");
    if (has_nul(raw)) return arg_wrong(g, "path", "a file path without NUL characters");
    *out = norm_path(&g->ta, raw);
    return 1;
}

static int needs_ask(agent_t* g, unsigned bit) { return !g->cfg.auto_approve && !(g->always & bit); }

static void ask(agent_t* g, call_t* c, str_t desc, int next_phase) {
    act_t* a = emit_op(g, ACT_ASK, c->name, desc, str_of(0, 0));
    a->id = g->tc.op = new_id(g);
    g->tc.phase = next_phase;
}

// The answer to a question. 1 to go on; 0 if the call was ended (the user said no).
static int answered(agent_t* g, const ev_t* ev, unsigned bit) {
    if (ev->status == ANS_NO) {
        fail(g, "The user declined this action. Do not try the same thing again; ask them what they would like instead.");
        return 0;
    }
    if (ev->status == ANS_ALWAYS) g->always |= bit;
    return 1;
}

static void say_tool(agent_t* g, const char* name, str_t what) {
    sb_t b;
    sb_init(&b, &g->tmp);
    sb_putz(&b, name);
    sb_putc(&b, ' ');
    sb_puts(&b, what);
    say(g, SAY_TOOL, sb_str(&b));
}

// A file-system failure in words the model can act on.
static void fs_fail(agent_t* g, const char* verb, str_t path, const ev_t* ev) {
    sb_t b;
    sb_init(&b, &g->tmp);
    switch (ev->status) {
    case FS_NOTFOUND:
        sb_putz(&b, "No such file: "); sb_puts(&b, path);
        sb_putz(&b, ". Paths are relative to the working directory; check with bash (ls, find).");
        break;
    case FS_ISDIR:
        sb_puts(&b, path); sb_putz(&b, " is a directory, not a file. List it with bash (ls).");
        break;
    case FS_TOOBIG:
        sb_puts(&b, path); sb_putz(&b, " is too large to read whole");
        if (ev->note.n) { sb_putz(&b, " ("); sb_puts(&b, ev->note); sb_putc(&b, ')'); }
        sb_putz(&b, ". Look at parts of it with bash (head, tail, grep -n, sed -n).");
        break;
    case FS_DENIED:
        sb_putz(&b, "Permission denied "); sb_putz(&b, verb); sb_putc(&b, ' '); sb_puts(&b, path); sb_putc(&b, '.');
        break;
    default:
        sb_putz(&b, "Could not "); sb_putz(&b, verb); sb_putc(&b, ' '); sb_puts(&b, path);
        if (ev->note.n) { sb_putz(&b, ": "); sb_puts(&b, ev->note); }
        break;
    }
    fail_sb(g, &b);
}

// Up to `max_lines` lines of `s`, each after `prefix`, cut at 120 characters; for the user's question.
static void preview(sb_t* b, const char* prefix, str_t s, int max_lines) {
    int lines = 0;
    size_t i = 0;
    while (i < s.n && lines < max_lines) {
        size_t j = i;
        while (j < s.n && s.p[j] != '\n') j++;
        sb_putz(b, prefix);
        sb_put(b, s.p + i, j - i > 120 ? 120 : j - i);
        if (j - i > 120) sb_putz(b, " ...");
        sb_putc(b, '\n');
        lines++;
        i = j + 1;
    }
    if (i < s.n) sb_putz(b, "  ... (more)\n");
}

static uint64_t count_lines(str_t s) {
    uint64_t n = 0;
    for (size_t i = 0; i < s.n; i++) if (s.p[i] == '\n') n++;
    if (s.n && s.p[s.n - 1] != '\n') n++;
    return n;
}

// ---- read_file ----

static void read_done(agent_t* g, call_t* c, const ev_t* ev) {
    (void)c;
    if (ev->status != FS_OK) { fs_fail(g, "read", g->tc.path, ev); return; }
    str_t d = ev->data;
    seen_add(g, g->tc.path);
    size_t probe = d.n < 8192 ? d.n : 8192;
    if (memchr(d.p, 0, probe)) {
        sb_t b;
        sb_init(&b, &g->tmp);
        sb_puts(&b, g->tc.path);
        sb_putz(&b, " looks like a binary file (it contains NUL bytes), so it was not read. Use bash (file, xxd, strings) if you need to look at it.");
        fail_sb(g, &b);
        return;
    }
    if (d.n == 0) { finish_call(g, str_z("(the file is empty)"), 0, "empty file"); return; }

    uint64_t total = count_lines(d);
    int64_t offset = g->tc.n, limit = g->tc.n2;
    if ((uint64_t)offset > total) {
        sb_t b;
        sb_init(&b, &g->tmp);
        sb_putz(&b, "offset ");
        sb_puti(&b, offset);
        sb_putz(&b, " is past the end of the file, which has ");
        sb_putu(&b, total);
        sb_putz(&b, " lines.");
        fail_sb(g, &b);
        return;
    }
    sb_t out;
    sb_init(&out, &g->tmp);
    uint64_t lineno = 0, shown = 0, last_shown = 0;
    int truncated_bytes = 0;
    size_t i = 0;
    while (i < d.n) {
        size_t j = i;
        while (j < d.n && d.p[j] != '\n') j++;
        lineno++;
        if ((int64_t)lineno >= offset) {
            if ((int64_t)shown >= limit) break;
            if (out.n >= READ_MAX_BYTES) { truncated_bytes = 1; break; }
            size_t len = j - i;
            if (len && d.p[i + len - 1] == '\r') len--;  // CRLF files read as plain lines
            sb_pad_u(&out, lineno, 6);
            sb_putc(&out, '\t');
            if (len > READ_MAX_LINE_BYTES) {
                size_t cut = READ_MAX_LINE_BYTES;
                while (cut > 0 && ((unsigned char)d.p[i + cut] & 0xC0) == 0x80) cut--;
                sb_put(&out, d.p + i, cut);
                sb_putz(&out, " ... [line cut: it is ");
                sb_putu(&out, len);
                sb_putz(&out, " bytes long]");
            } else {
                sb_put(&out, d.p + i, len);
            }
            sb_putc(&out, '\n');
            shown++;
            last_shown = lineno;
        }
        i = j + 1;
    }
    if (last_shown < total) {
        sb_putz(&out, "\n(showing lines ");
        sb_putu(&out, (uint64_t)offset);
        sb_putc(&out, '-');
        sb_putu(&out, last_shown);
        sb_putz(&out, " of ");
        sb_putu(&out, total);
        sb_putz(&out, truncated_bytes ? "; stopped at the output size limit" : "");
        sb_putz(&out, "; call read_file again with offset ");
        sb_putu(&out, last_shown + 1);
        sb_putz(&out, " to continue)");
    }
    sb_t sum;
    sb_init(&sum, &g->tmp);
    sb_putu(&sum, shown);
    sb_putz(&sum, " lines");
    finish_call(g, sb_str(&out), 0, sb_str(&sum).p);
}

static void t_read(agent_t* g, call_t* c, const ev_t* ev) {
    if (!ev) {
        if (!input_ok(g, c)) return;
        str_t path;
        int64_t offset = 1, limit = READ_DEFAULT_LINES;
        int p1, p2;
        if (!arg_path(g, c, &path)) return;
        if (!arg_int(g, c, "offset", 0, &offset, &p1)) return;
        if (!arg_int(g, c, "limit", 0, &limit, &p2)) return;
        if (!p1 || offset < 1) offset = 1;
        if (!p2 || limit < 1) limit = READ_DEFAULT_LINES;
        if (limit > READ_MAX_LINES) limit = READ_MAX_LINES;
        g->tc.path = path;
        g->tc.n = offset;
        g->tc.n2 = limit;
        say_tool(g, "read_file", path);
        act_t* a = emit_op(g, ACT_READ, path, str_of(0, 0), str_of(0, 0));
        a->id = g->tc.op = new_id(g);
        g->tc.phase = 1;
        return;
    }
    read_done(g, c, ev);
}

// ---- write_file ----

enum { W_CHECKED = 1, W_ASKED, W_WROTE };

static void t_write(agent_t* g, call_t* c, const ev_t* ev) {
    tc_t* t = &g->tc;
    if (!ev) {
        if (!input_ok(g, c)) return;
        str_t path, content;
        if (!arg_path(g, c, &path)) return;
        if (!arg_str(g, c, "content", 1, &content)) return;
        if (!content.p) content = str_dup(&g->ta, str_z(""));
        t->path = path;
        t->data = content;
        sb_t w;
        sb_init(&w, &g->tmp);
        sb_puts(&w, path);
        sb_putz(&w, " (");
        sb_putu(&w, content.n);
        sb_putz(&w, " bytes)");
        say_tool(g, "write_file", sb_str(&w));
        // Look at what is there first: never silently replace a file the model has not read.
        act_t* a = emit_op(g, ACT_READ, path, str_of(0, 0), str_of(0, 0));
        a->id = t->op = new_id(g);
        t->phase = W_CHECKED;
        return;
    }
    switch (t->phase) {
    case W_CHECKED: {
        t->flag = 0;  // does it exist
        if (ev->status == FS_OK) {
            t->flag = 1;
            t->n = (int64_t)ev->data.n;
            if (!seen_has(g, t->path)) {
                sb_t b;
                sb_init(&b, &g->tmp);
                sb_puts(&b, t->path);
                sb_putz(&b, " already exists and you have not read it in this session. Read it first, so you do not overwrite something you have not seen, or use edit_file for a targeted change.");
                fail_sb(g, &b);
                return;
            }
        } else if (ev->status == FS_ISDIR) {
            fs_fail(g, "write", t->path, ev);
            return;
        } else if (ev->status != FS_NOTFOUND && ev->status != FS_TOOBIG) {
            fs_fail(g, "write", t->path, ev);
            return;
        } else if (ev->status == FS_TOOBIG) {
            t->flag = 1;
            if (!seen_has(g, t->path)) { fs_fail(g, "write", t->path, ev); return; }
        }
        if (needs_ask(g, BIT_WRITE)) {
            sb_t d;
            sb_init(&d, &g->tmp);
            sb_putz(&d, t->flag ? "overwrite " : "create ");
            sb_puts(&d, t->path);
            sb_putz(&d, " (");
            sb_putu(&d, t->data.n);
            sb_putz(&d, " bytes)\n");
            preview(&d, "+ ", t->data, 6);
            ask(g, c, sb_str(&d), W_ASKED);
            return;
        }
        // fall through to the write
    }
    /* fallthrough */
    case W_ASKED:
        if (t->phase == W_ASKED && !answered(g, ev, BIT_WRITE)) return;
        {
            act_t* a = emit_op(g, ACT_WRITE, t->path, t->data, str_of(0, 0));
            a->id = t->op = new_id(g);
            t->phase = W_WROTE;
        }
        return;
    case W_WROTE: {
        if (ev->status != FS_OK) { fs_fail(g, "write", t->path, ev); return; }
        seen_add(g, t->path);
        sb_t b;
        sb_init(&b, &g->tmp);
        sb_putz(&b, t->flag ? "Replaced " : "Created ");
        sb_puts(&b, t->path);
        sb_putz(&b, " (");
        sb_putu(&b, t->data.n);
        sb_putz(&b, " bytes).");
        finish_call(g, sb_str(&b), 0, t->flag ? "replaced" : "created");
        return;
    }
    }
}

// ---- edit_file ----

enum { E_READ = 1, E_ASKED, E_WROTE };

static uint64_t line_at(str_t s, size_t off) {
    uint64_t n = 1;
    for (size_t i = 0; i < off && i < s.n; i++) if (s.p[i] == '\n') n++;
    return n;
}

// A few numbered lines around the edit, so the model can see the result without another read.
static void snippet(sb_t* b, str_t s, uint64_t from_line, uint64_t to_line) {
    uint64_t line = 1, shown = 0;
    size_t i = 0;
    while (i <= s.n && line <= to_line && shown < 24) {
        size_t j = i;
        while (j < s.n && s.p[j] != '\n') j++;
        if (line >= from_line && !(i == s.n)) {
            sb_pad_u(b, line, 6);
            sb_putc(b, '\t');
            size_t len = j - i;
            sb_put(b, s.p + i, len > 200 ? 200 : len);
            sb_putc(b, '\n');
            shown++;
        }
        if (j >= s.n) break;
        i = j + 1;
        line++;
    }
}

static void t_edit(agent_t* g, call_t* c, const ev_t* ev) {
    tc_t* t = &g->tc;
    if (!ev) {
        if (!input_ok(g, c)) return;
        str_t path, olds, news;
        int all;
        if (!arg_path(g, c, &path)) return;
        if (!arg_str(g, c, "old_string", 1, &olds)) return;
        if (!arg_str(g, c, "new_string", 1, &news)) return;
        if (!arg_bool(g, c, "replace_all", &all)) return;
        if (!olds.p) olds = str_dup(&g->ta, str_z(""));
        if (!news.p) news = str_dup(&g->ta, str_z(""));
        if (olds.n == 0) {
            fail(g, "old_string must not be empty. To create a new file use write_file; to change a file give the exact text to replace.");
            return;
        }
        if (str_eq(olds, news)) { fail(g, "old_string and new_string are identical, so there is nothing to change."); return; }
        t->path = path; t->s1 = olds; t->s2 = news; t->flag = all;
        say_tool(g, "edit_file", path);
        act_t* a = emit_op(g, ACT_READ, path, str_of(0, 0), str_of(0, 0));
        a->id = t->op = new_id(g);
        t->phase = E_READ;
        return;
    }
    switch (t->phase) {
    case E_READ: {
        if (ev->status != FS_OK) {
            if (ev->status == FS_NOTFOUND) {
                sb_t b;
                sb_init(&b, &g->tmp);
                sb_putz(&b, "No such file: ");
                sb_puts(&b, t->path);
                sb_putz(&b, ". edit_file only changes existing files; to create one use write_file.");
                fail_sb(g, &b);
            } else {
                fs_fail(g, "edit", t->path, ev);
            }
            return;
        }
        if (!seen_has(g, t->path)) {
            sb_t b;
            sb_init(&b, &g->tmp);
            sb_putz(&b, "You have not read ");
            sb_puts(&b, t->path);
            sb_putz(&b, " in this session. Read it first with read_file (so your old_string comes from the real text), then edit.");
            fail_sb(g, &b);
            return;
        }
        str_t file = ev->data, olds = t->s1, news = t->s2;
        // Find every occurrence.
        uint64_t count = 0;
        size_t first_at = STR_NONE;
        sb_t where;
        sb_init(&where, &g->tmp);
        for (size_t pos = 0; pos <= file.n;) {
            size_t k = str_find(str_of(file.p + pos, file.n - pos), olds);
            if (k == STR_NONE) break;
            if (count == 0) first_at = pos + k;
            if (count < 6) {
                if (count) sb_putz(&where, ", ");
                sb_putu(&where, line_at(file, pos + k));
            }
            count++;
            pos += k + olds.n;
        }
        if (count == 0) {
            sb_t b;
            sb_init(&b, &g->tmp);
            sb_putz(&b, "old_string was not found in ");
            sb_puts(&b, t->path);
            sb_putz(&b, ". It must match the file exactly, including whitespace, indentation and line endings.");
            str_t trimmed = str_trim(olds);
            if (trimmed.n && trimmed.n != olds.n && str_find(file, trimmed) != STR_NONE)
                sb_putz(&b, " The text is there, but the whitespace around it differs from what you gave.");
            else if (memchr(file.p, '\r', file.n) && !memchr(olds.p, '\r', olds.n) && memchr(olds.p, '\n', olds.n))
                sb_putz(&b, " This file uses CRLF line endings; a multi-line old_string has to include the carriage returns.");
            else
                sb_putz(&b, " Re-read the lines you want to change and copy them exactly.");
            fail_sb(g, &b);
            return;
        }
        if (count > 1 && !t->flag) {
            sb_t b;
            sb_init(&b, &g->tmp);
            sb_putz(&b, "old_string appears ");
            sb_putu(&b, count);
            sb_putz(&b, " times in ");
            sb_puts(&b, t->path);
            sb_putz(&b, " (at lines ");
            sb_puts(&b, sb_str(&where));
            sb_putz(&b, count > 6 ? ", ...)" : ")");
            sb_putz(&b, ". Add some surrounding lines to old_string so it matches exactly one place, or set replace_all to true to change every occurrence.");
            fail_sb(g, &b);
            return;
        }
        // Build the new file.
        sb_t out;
        sb_init(&out, &g->ta);
        size_t pos = 0;
        uint64_t done = 0;
        size_t new_first = 0;
        while (pos <= file.n) {
            size_t k = str_find(str_of(file.p + pos, file.n - pos), olds);
            if (k == STR_NONE || (done && !t->flag)) break;
            sb_put(&out, file.p + pos, k);
            if (done == 0) new_first = out.n;
            sb_puts(&out, news);
            pos += k + olds.n;
            done++;
        }
        sb_put(&out, file.p + pos, file.n - pos);
        str_t result = sb_str(&out);
        if (!result.p) { fail(g, "Out of memory while editing."); return; }
        t->data = result;
        t->n = (int64_t)done;
        t->n2 = (int64_t)line_at(result, new_first);
        (void)first_at;
        if (needs_ask(g, BIT_EDIT)) {
            sb_t d;
            sb_init(&d, &g->tmp);
            sb_putz(&d, "edit ");
            sb_puts(&d, t->path);
            sb_putz(&d, " (");
            sb_putu(&d, done);
            sb_putz(&d, done == 1 ? " place)\n" : " places)\n");
            preview(&d, "- ", olds, 6);
            preview(&d, "+ ", news, 6);
            ask(g, c, sb_str(&d), E_ASKED);
            return;
        }
        t->phase = E_ASKED;
    }
    /* fallthrough */
    case E_ASKED:
        if (ev->kind == EV_ANSWER && !answered(g, ev, BIT_EDIT)) return;
        {
            act_t* a = emit_op(g, ACT_WRITE, t->path, t->data, str_of(0, 0));
            a->id = t->op = new_id(g);
            t->phase = E_WROTE;
        }
        return;
    case E_WROTE: {
        if (ev->status != FS_OK) { fs_fail(g, "write", t->path, ev); return; }
        sb_t b;
        sb_init(&b, &g->tmp);
        sb_putz(&b, "Edited ");
        sb_puts(&b, t->path);
        sb_putz(&b, ": replaced ");
        sb_putu(&b, (uint64_t)t->n);
        sb_putz(&b, t->n == 1 ? " occurrence." : " occurrences.");
        uint64_t added = count_lines(t->s2);
        uint64_t from = t->n2 > 2 ? (uint64_t)t->n2 - 2 : 1;
        sb_putz(&b, " The first change now reads:\n");
        snippet(&b, t->data, from, (uint64_t)t->n2 + (added ? added - 1 : 0) + 2);
        finish_call(g, sb_str(&b), 0, "edited");
        return;
    }
    }
}

// ---- bash ----

// The length of the well-formed UTF-8 sequence of two or more bytes at s[0..n), or 0 if there is none. C1 control
// characters (U+0080 to U+009F) count as none: some terminals obey them.
static size_t utf8_seq(const unsigned char* s, size_t n) {
    if (n >= 2 && s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) return (s[0] == 0xC2 && s[1] < 0xA0) ? 0 : 2;
    if (n >= 3 && s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;   // overlong
        if (s[0] == 0xED && s[1] >= 0xA0) return 0;  // surrogate
        return 3;
    }
    if (n >= 4 && s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;   // overlong
        if (s[0] == 0xF4 && s[1] >= 0x90) return 0;  // past U+10FFFF
        return 4;
    }
    return 0;
}

// Output the user should see: up to SHOW_LINES lines of a command's output, each cut at SHOW_COLS bytes, with
// control characters and malformed bytes made harmless (the output is whatever the command printed, and it goes to a
// terminal). The model gets the real output; this is only the preview.
static void say_output(agent_t* g, str_t out) {
    enum { SHOW_LINES = 20, SHOW_COLS = 120 };
    if (out.n == 0) return;
    int total = 0;
    for (size_t k = 0; k < out.n; k++) if (out.p[k] == '\n') total++;
    if (out.p[out.n - 1] != '\n') total++;
    sb_t b;
    sb_init(&b, &g->tmp);
    int shown = 0;
    size_t i = 0;
    while (i < out.n && shown < SHOW_LINES) {
        size_t j = i;
        while (j < out.n && out.p[j] != '\n') j++;
        if (shown) sb_putc(&b, '\n');
        sb_putz(&b, "  | ");
        size_t k = i;
        while (k < j) {
            unsigned char ch = (unsigned char)out.p[k];
            size_t len = ch >= 0x80 ? utf8_seq((const unsigned char*)out.p + k, j - k) : 1;
            if (k - i + (len ? len : 1) > SHOW_COLS) break;
            if (len > 1) sb_put(&b, out.p + k, len);
            else sb_putc(&b, (len == 1 && (ch == '\t' || (ch >= 32 && ch < 127))) ? (char)ch : '?');
            k += len ? len : 1;
        }
        if (k < j) sb_putz(&b, " ...");
        shown++;
        i = j + 1;
    }
    if (total > shown) {
        sb_putz(&b, "\n  | ... ");
        sb_puti(&b, total - shown);
        sb_putz(&b, total - shown == 1 ? " more line" : " more lines");
    }
    say(g, SAY_TOOL, sb_str(&b));
}


enum { B_ASKED = 1, B_RUNNING };

static void t_bash(agent_t* g, call_t* c, const ev_t* ev) {
    tc_t* t = &g->tc;
    if (!ev) {
        if (!input_ok(g, c)) return;
        str_t cmd, desc;
        int64_t timeout = BASH_DEFAULT_MS;
        int has_t, bg;
        if (!arg_str(g, c, "command", 1, &cmd)) return;
        if (cmd.n == 0 || str_trim(cmd).n == 0) { fail(g, "The command is empty."); return; }
        if (has_nul(cmd)) { arg_wrong(g, "command", "text without NUL characters"); return; }
        if (!arg_str(g, c, "description", 0, &desc)) return;
        if (!arg_int(g, c, "timeout_ms", 0, &timeout, &has_t)) return;
        if (!arg_bool(g, c, "run_in_background", &bg)) return;
        if (!has_t || timeout < 1000) timeout = has_t ? 1000 : BASH_DEFAULT_MS;
        if (timeout > BASH_MAX_MS) timeout = BASH_MAX_MS;
        t->s1 = cmd;
        t->n = timeout;
        t->flag = bg;
        sb_t w;
        sb_init(&w, &g->tmp);
        if (bg) sb_putz(&w, "(background) ");
        sb_puts(&w, desc.n ? desc : first_line(cmd, 100));
        say_tool(g, "bash", sb_str(&w));
        if (!needs_ask(g, BIT_BASH)) {
            // Nobody is being asked, so the command itself goes on the screen: the title above is the model's own words.
            sb_t shown;
            sb_init(&shown, &g->tmp);
            sb_putz(&shown, "  $ ");
            sb_puts(&shown, first_line(cmd, 120));
            say(g, SAY_TOOL, sb_str(&shown));
        }
        if (needs_ask(g, BIT_BASH)) {
            sb_t d;
            sb_init(&d, &g->tmp);
            sb_putz(&d, bg ? "run in the background:\n" : "run:\n");
            sb_puts(&d, cmd);
            if (desc.n) { sb_putz(&d, "\n("); sb_puts(&d, desc); sb_putc(&d, ')'); }
            ask(g, c, sb_str(&d), B_ASKED);
            return;
        }
        t->phase = B_ASKED;
        ev = 0;
    } else if (t->phase == B_ASKED) {
        if (!answered(g, ev, BIT_BASH)) return;
    }

    if (t->phase == B_ASKED) {
        if (t->flag) {
            int64_t id = new_id(g);
            sb_t lp;
            sb_init(&lp, &g->life);
            sb_putz(&lp, ".barb/tasks/");
            sb_puti(&lp, id);
            sb_putz(&lp, ".log");
            task_t* task = arena_alloc(&g->life, sizeof *task);
            if (!task) { fail(g, "Out of memory."); return; }
            task->id = id;
            task->cmd = str_dup(&g->life, t->s1);
            task->log = sb_str(&lp);
            task->next = g->tasks;
            g->tasks = task;
            act_t* a = emit_op(g, ACT_SPAWN, t->s1, str_of(0, 0), task->log);
            a->id = id;
            a->flag = 1;
            sb_t r;
            sb_init(&r, &g->tmp);
            sb_putz(&r, "Started background task ");
            sb_puti(&r, id);
            sb_putz(&r, ". Its output is written to ");
            sb_puts(&r, task->log);
            sb_putz(&r, " (read it with read_file or tail it with bash). You will be woken with a message when it exits; "
                        "you do not need to poll. If there is nothing else to do meanwhile, end your turn: waiting costs nothing.");
            sb_t s;
            sb_init(&s, &g->tmp);
            sb_putz(&s, "started task ");
            sb_puti(&s, id);
            finish_call(g, sb_str(&r), 0, sb_str(&s).p);
            return;
        }
        act_t* a = emit_op(g, ACT_SPAWN, t->s1, str_of(0, 0), str_of(0, 0));
        a->id = t->op = new_id(g);
        a->n = t->n;
        t->phase = B_RUNNING;
        return;
    }

    // B_RUNNING: the process ended.
    sb_t r;
    sb_init(&r, &g->tmp);
    str_t out = cap_output(&g->tmp, ev->data, OUTPUT_MAX);
    if (ev->flags & PROC_FAILED) {
        sb_putz(&r, "The command could not be started");
        if (ev->note.n) { sb_putz(&r, ": "); sb_puts(&r, ev->note); }
        finish_call(g, sb_str(&r), 1, 0);
        return;
    }
    say_output(g, out);
    if (out.n) { sb_puts(&r, out); if (out.p[out.n - 1] != '\n') sb_putc(&r, '\n'); }
    else sb_putz(&r, "(no output)\n");
    int is_error = 0;
    if (ev->flags & PROC_TIMEDOUT) {
        // The limit as the model gave it: 1.5 seconds, not "1 seconds".
        sb_putz(&r, "[killed: it ran longer than its limit of ");
        sb_puti(&r, t->n / 1000);
        if (t->n % 1000) { sb_putc(&r, '.'); sb_puti(&r, t->n % 1000 / 100); }
        sb_putz(&r, t->n == 1000 ? " second" : " seconds");
        sb_putz(&r, ". Use run_in_background for long commands, or a longer timeout_ms.]");
        is_error = 1;
    } else if (ev->flags & PROC_SIGNALED) {
        sb_putz(&r, "[killed by a signal]");
    } else {
        sb_putz(&r, "[exit code: ");
        sb_puti(&r, ev->status);
        sb_putc(&r, ']');
    }
    sb_t s;
    sb_init(&s, &g->tmp);
    if (ev->flags & PROC_TIMEDOUT) sb_putz(&s, "timed out");
    else if (ev->flags & PROC_SIGNALED) sb_putz(&s, "killed");
    else { sb_putz(&s, "exit "); sb_puti(&s, ev->status); }
    finish_call(g, sb_str(&r), is_error, sb_str(&s).p);
}

// ---- task_stop and schedule_wakeup ----

static void t_task_stop(agent_t* g, call_t* c) {
    if (!input_ok(g, c)) return;
    int64_t id;
    int present;
    if (!arg_int(g, c, "task_id", 1, &id, &present)) return;
    for (task_t* t = g->tasks; t; t = t->next) {
        if (t->id != id) continue;
        act_t* a = emit_op(g, ACT_CANCEL, str_of(0, 0), str_of(0, 0), str_of(0, 0));
        a->id = id;
        sb_t r;
        sb_init(&r, &g->tmp);
        sb_putz(&r, "Asked task ");
        sb_puti(&r, id);
        sb_putz(&r, " to stop. You will get a message when it has exited.");
        finish_call(g, sb_str(&r), 0, "stopping task");
        return;
    }
    sb_t r;
    sb_init(&r, &g->tmp);
    sb_putz(&r, "No running background task has the number ");
    sb_puti(&r, id);
    sb_putz(&r, ".");
    if (g->tasks) {
        sb_putz(&r, " Running now:");
        for (task_t* t = g->tasks; t; t = t->next) { sb_putc(&r, ' '); sb_puti(&r, t->id); }
        sb_putc(&r, '.');
    } else {
        sb_putz(&r, " None are running.");
    }
    fail_sb(g, &r);
}

static void t_wakeup(agent_t* g, call_t* c) {
    if (!input_ok(g, c)) return;
    int64_t secs;
    int present;
    str_t note;
    if (!arg_int(g, c, "seconds", 1, &secs, &present)) return;
    if (!arg_str(g, c, "note", 0, &note)) return;
    if (secs < 1 || secs > 86400) { fail(g, "seconds must be between 1 and 86400."); return; }
    wake_t* w = arena_alloc(&g->life, sizeof *w);
    if (!w) { fail(g, "Out of memory."); return; }
    w->id = new_id(g);
    w->note = str_dup(&g->life, note.p ? note : str_z(""));
    w->next = g->wakes;
    g->wakes = w;
    act_t* a = emit_op(g, ACT_TIMER, str_of(0, 0), str_of(0, 0), str_of(0, 0));
    a->id = w->id;
    a->n = secs * 1000;
    sb_t r;
    sb_init(&r, &g->tmp);
    sb_putz(&r, "Timer set: you will be woken in ");
    sb_puti(&r, secs);
    sb_putz(&r, " seconds with a message. End your turn now if there is nothing else to do; waiting costs nothing. "
                "(The timer only runs while barb is running.)");
    sb_t s;
    sb_init(&s, &g->tmp);
    sb_putz(&s, "wake in ");
    sb_puti(&s, secs);
    sb_putz(&s, "s");
    finish_call(g, sb_str(&r), 0, sb_str(&s).p);
}

// ---- dispatch ----

static void run_tool(agent_t* g, call_t* c, const ev_t* ev) {
    if (str_eqz(c->name, "read_file")) t_read(g, c, ev);
    else if (str_eqz(c->name, "write_file")) t_write(g, c, ev);
    else if (str_eqz(c->name, "edit_file")) t_edit(g, c, ev);
    else if (str_eqz(c->name, "bash")) t_bash(g, c, ev);
    else if (str_eqz(c->name, "task_stop")) t_task_stop(g, c);
    else if (str_eqz(c->name, "schedule_wakeup")) t_wakeup(g, c);
    else {
        sb_t b;
        sb_init(&b, &g->tmp);
        sb_putz(&b, "There is no tool named '");
        sb_puts(&b, c->name);
        sb_putz(&b, "'. The tools are read_file, write_file, edit_file, bash, task_stop and schedule_wakeup.");
        fail_sb(g, &b);
    }
}

void tool_begin(agent_t* g, call_t* c) { run_tool(g, c, 0); }
void tool_event(agent_t* g, call_t* c, const ev_t* ev) { run_tool(g, c, ev); }

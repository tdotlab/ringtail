// The transcript and the Messages API: see api.h.
#include "api.h"
#include <string.h>

block_t* block_new(arena_t* a, bkind_t kind) {
    block_t* b = arena_alloc(a, sizeof *b);
    if (b) b->kind = kind;
    return b;
}

msg_t* tr_new_msg(arena_t* a, transcript_t* t, int assistant) {
    msg_t* m = arena_alloc(a, sizeof *m);
    if (!m) return 0;
    m->assistant = assistant;
    if (t->last) t->last->next = m; else t->first = m;
    t->last = m;
    return m;
}

void msg_add(msg_t* m, block_t* b) {
    b->next = 0;
    if (m->last) m->last->next = b; else m->first = b;
    m->last = b;
}

msg_t* tr_user_msg(arena_t* a, transcript_t* t) {
    if (t->last && !t->last->assistant) return t->last;
    return tr_new_msg(a, t, 0);
}

int tr_add_text(arena_t* a, msg_t* m, str_t text) {
    if (text.n == 0) return 1;
    block_t* b = block_new(a, BK_TEXT);
    if (!b) return 0;
    b->text = str_dup(a, text);
    if (!b->text.p) return 0;
    msg_add(m, b);
    return 1;
}

// ---- the request ----

static int emittable(const block_t* b) {
    switch (b->kind) {
    case BK_TEXT: return b->text.n > 0;
    case BK_TOOL_USE: case BK_TOOL_RESULT: return 1;
    case BK_RAW: return json_valid(b->text);
    }
    return 0;
}

static void cache_mark(jw_t* w) {
    jw_key(w, "cache_control");
    jw_obj(w);
    jw_key(w, "type"); jw_strz(w, "ephemeral");
    jw_obj_end(w);
}

static void emit_block(jw_t* w, const block_t* b, int cache) {
    if (b->kind == BK_RAW) { jw_raw(w, b->text); return; }
    jw_obj(w);
    switch (b->kind) {
    case BK_TEXT:
        jw_key(w, "type"); jw_strz(w, "text");
        jw_key(w, "text"); jw_str(w, b->text);
        if (cache) cache_mark(w);
        break;
    case BK_TOOL_USE:
        jw_key(w, "type"); jw_strz(w, "tool_use");
        jw_key(w, "id"); jw_str(w, b->id);
        jw_key(w, "name"); jw_str(w, b->name);
        jw_key(w, "input"); jw_raw(w, b->input.n ? b->input : str_z("{}"));
        break;
    case BK_TOOL_RESULT:
        jw_key(w, "type"); jw_strz(w, "tool_result");
        jw_key(w, "tool_use_id"); jw_str(w, b->id);
        jw_key(w, "content"); jw_str(w, b->text);
        if (b->is_error) { jw_key(w, "is_error"); jw_bool(w, 1); }
        if (cache) cache_mark(w);
        break;
    case BK_RAW:
        break;
    }
    jw_obj_end(w);
}

int api_request(sb_t* out, const api_cfg_t* cfg, const transcript_t* t) {
    // The last block of the last message that has any gets the cache breakpoint.
    const msg_t* last_msg = 0;
    const block_t* last_block = 0;
    for (const msg_t* m = t->first; m; m = m->next)
        for (const block_t* b = m->first; b; b = b->next)
            if (emittable(b)) { last_msg = m; last_block = b; }

    jw_t w;
    jw_init(&w, out);
    jw_obj(&w);
    jw_key(&w, "model"); jw_str(&w, cfg->model);
    jw_key(&w, "max_tokens"); jw_int(&w, cfg->max_tokens);
    if (cfg->system.n) {
        jw_key(&w, "system");
        jw_arr(&w);
        jw_obj(&w);
        jw_key(&w, "type"); jw_strz(&w, "text");
        jw_key(&w, "text"); jw_str(&w, cfg->system);
        cache_mark(&w);
        jw_obj_end(&w);
        jw_arr_end(&w);
    }
    if (cfg->tools.n) { jw_key(&w, "tools"); jw_raw(&w, cfg->tools); }
    jw_key(&w, "messages");
    jw_arr(&w);
    for (const msg_t* m = t->first; m; m = m->next) {
        int any = 0;
        for (const block_t* b = m->first; b; b = b->next) if (emittable(b)) any = 1;
        if (!any) continue;
        jw_obj(&w);
        jw_key(&w, "role"); jw_strz(&w, m->assistant ? "assistant" : "user");
        jw_key(&w, "content");
        jw_arr(&w);
        for (const block_t* b = m->first; b; b = b->next) {
            if (!emittable(b)) continue;
            emit_block(&w, b, b == last_block && m == last_msg &&
                              (b->kind == BK_TEXT || b->kind == BK_TOOL_RESULT));
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    return !out->failed && !w.bad;
}

// ---- the reply ----

static int get_str(arena_t* a, str_t obj, const char* key, str_t* out) {
    str_t v;
    if (!json_get(obj, key, &v)) return 0;
    return json_as_str(a, v, out);
}

static int64_t get_int(str_t obj, const char* key) {
    str_t v;
    int64_t n = 0;
    if (json_get(obj, key, &v) && json_as_int(v, &n)) return n;
    return 0;
}

static int parse_content(arena_t* a, str_t content, msg_t* m) {
    json_t j;
    json_open(&j, content);
    if (json_next(&j) != JT_ARR) return 0;
    for (;;) {
        jtok_t t = json_next(&j);
        if (t == JT_ARR_END) return 1;
        str_t el;
        if (t != JT_OBJ || !json_capture(&j, t, &el)) return 0;

        str_t type = str_of(0, 0);
        get_str(a, el, "type", &type);
        if (str_eqz(type, "text")) {
            str_t text;
            if (!get_str(a, el, "text", &text)) return 0;
            if (!tr_add_text(a, m, text)) return 0;
        } else if (str_eqz(type, "tool_use")) {
            block_t* b = block_new(a, BK_TOOL_USE);
            str_t input;
            if (!b) return 0;
            if (!get_str(a, el, "id", &b->id) || !get_str(a, el, "name", &b->name)) return 0;
            if (!json_get(el, "input", &input)) input = str_z("{}");
            b->input = str_dup(a, input);
            if (!b->input.p) return 0;
            msg_add(m, b);
        } else {
            block_t* b = block_new(a, BK_RAW);
            if (!b) return 0;
            b->text = str_dup(a, el);
            if (!b->text.p) return 0;
            msg_add(m, b);
        }
    }
}

int api_parse(arena_t* a, str_t body, api_reply_t* r) {
    memset(r, 0, sizeof *r);
    if (!json_valid(body)) return 0;
    str_t type;
    if (!get_str(a, body, "type", &type)) return 0;

    if (str_eqz(type, "error")) {
        str_t e;
        if (!json_get(body, "error", &e)) return 0;
        get_str(a, e, "type", &r->err_type);
        get_str(a, e, "message", &r->err_msg);
        return 1;
    }
    if (!str_eqz(type, "message")) return 0;

    str_t content;
    if (!json_get(body, "content", &content)) return 0;
    msg_t* m = arena_alloc(a, sizeof *m);
    if (!m) return 0;
    m->assistant = 1;
    if (!parse_content(a, content, m)) return 0;
    r->msg = m;
    r->is_message = 1;
    get_str(a, body, "stop_reason", &r->stop_reason);  // null (an unfinished stream) leaves it empty
    str_t u;
    if (json_get(body, "usage", &u)) {
        r->in_tok = get_int(u, "input_tokens");
        r->out_tok = get_int(u, "output_tokens");
        r->cache_read = get_int(u, "cache_read_input_tokens");
        r->cache_write = get_int(u, "cache_creation_input_tokens");
    }
    return !a->failed;
}

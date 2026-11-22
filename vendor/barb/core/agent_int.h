// What agent.c and tools.c share. Not for the platform.
#ifndef BARB_AGENT_INT_H
#define BARB_AGENT_INT_H

#include "agent.h"
#include "api.h"

typedef enum { S_IDLE, S_MODEL, S_RETRY, S_TOOLS, S_LOAD } gstate_t;

typedef struct {
    str_t id, name, input;
    int done, is_error;
    str_t result;
} call_t;

typedef struct note {
    struct note* next;
    str_t text;
    int from_user;
} note_t;

typedef struct task {
    struct task* next;
    int64_t id;
    str_t cmd, log;
} task_t;

typedef struct wake {
    struct wake* next;
    int64_t id;
    str_t note;
} wake_t;

// The tool call being run. Its strings live in agent->ta, which is emptied when the call ends.
typedef struct {
    int phase;       // PH_DONE once finish_call has been called
    int64_t op;      // the operation the tool is waiting on, or 0
    str_t path, s1, s2, data;
    int flag;
    int64_t n, n2;
} tc_t;

#define PH_DONE (-1)

enum { BIT_WRITE = 1, BIT_EDIT = 2, BIT_BASH = 4 };

struct agent {
    agent_cfg_t cfg;
    arena_t life;          // the conversation and everything that lasts
    arena_t tmp;           // one step's work, emptied at the start of the next step
    arena_t ta;            // one tool call's work, emptied when the call ends
    transcript_t tr;
    api_cfg_t api;
    gstate_t state;
    int64_t next_id;

    int64_t http_id;       // the request in flight
    int retries;
    int64_t retry_timer;

    call_t* calls;         // the tool calls of the last reply
    int ncalls, cur;
    tc_t tc;

    note_t *nhead, *ntail; // text waiting to join the next user message
    task_t* tasks;         // background tasks still running
    wake_t* wakes;         // timers set with schedule_wakeup
    str_t* seen;           // files the model has read (or written) this session
    int nseen, capseen;
    unsigned always;       // BIT_*: the user said yes-always

    int ld_phase;          // LD_*: reading the project's instructions
    int64_t ld_op;         // the read in flight
    str_t ld_file;         // the file the instructions came from
    str_t ld_text;         // the text so far
    str_t ld_held;         // what the user typed that started the session; it goes once they are read
    str_t ld_imports[8];   // the "@file" lines it names, read one after another
    str_t ld_imptext[8];   // and what each one held
    int ld_nimports, ld_next;

    int turns;             // model calls since the user last spoke
    int limit_said;
    int wait_said;
    int quit_armed;
    int eof;
    int done;
    int failed;            // the last model call ended in an API error (the exit status when we finish)

    act_t* acts;
    size_t nacts, cap;
};

// services of agent.c
void say(agent_t* g, int style, str_t text);
void sayz(agent_t* g, int style, const char* z);
act_t* emit_op(agent_t* g, actk_t kind, str_t a, str_t b, str_t c);
int64_t new_id(agent_t* g);
// End the current tool call. `summary`, if given, is shown to the user.
void finish_call(agent_t* g, str_t result, int is_error, const char* summary);
int seen_has(const agent_t* g, str_t path);
void seen_add(agent_t* g, str_t path);

// tools.c
extern const char TOOLS_JSON[];
void tool_begin(agent_t* g, call_t* c);
void tool_event(agent_t* g, call_t* c, const ev_t* ev);
// A running background task or timer count, for the "waiting" line.
str_t cap_output(arena_t* a, str_t s, size_t max);

#endif

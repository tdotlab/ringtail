// The agent: a state machine that turns events into actions.
//
// Barb's core does no I/O. The platform (src/posix, later Windows and
// RingTail) feeds it events (a line the user typed, an HTTP reply, a file's
// contents, a process that exited, a timer that fired) and carries out the
// actions it returns (print this, POST that, read this file, run this
// command, wake me in 60 seconds). Between events the core is not running,
// and the platform is free to sleep until the next one. That is the whole
// mechanism behind "go to sleep on a long wait and wake up when it is done":
// a sleeping agent is one that gets no events and makes no model calls.
//
// Each operation the core starts (an HTTP request, a file read, a process,
// a timer, a permission question) has an id the core chose, and the event
// that answers it carries the same id. An event with an id the core is not
// waiting for (a reply to something it cancelled) is ignored.
//
// Memory: strings in the actions returned by agent_step stay valid until the
// next call to agent_step, so the platform copies what it keeps. Each one is
// followed by a NUL byte (not counted in n), so the platform may hand .p to
// a C function such as open(). Strings in an event are the platform's and
// need only live through the call.
#ifndef BARB_AGENT_H
#define BARB_AGENT_H

#include "arena.h"
#include "str.h"

typedef enum {
    EV_INPUT,      // data: a line the user typed
    EV_EOF,        // the user will type no more; finish what is pending, then stop
    EV_HTTP,       // id, status (0 if the request itself failed; note says why), data: the body
    EV_FILE,       // id, status (FS_*), data: contents for a read, note: what went wrong
    EV_ANSWER,     // id, status (ANS_*): the user's answer to an ACT_ASK
    EV_PROC,       // id, status: exit code, flags (PROC_*), data: output, ms: run time, note
    EV_TIMER,      // id
    EV_INTERRUPT   // the user pressed Ctrl-C
} evk_t;

enum { FS_OK = 0, FS_NOTFOUND, FS_ISDIR, FS_TOOBIG, FS_DENIED, FS_ERROR };
enum { ANS_NO = 0, ANS_YES, ANS_ALWAYS };
enum { PROC_TIMEDOUT = 1, PROC_SIGNALED = 2, PROC_FAILED = 4 };  // FAILED: it never started

typedef struct {
    evk_t kind;
    int64_t id;
    int status;
    int flags;
    str_t data;
    str_t note;
    int64_t ms;
} ev_t;

typedef enum {
    ACT_SAY,      // flag: SAY_*, a: the text to show the user
    ACT_HTTP,     // id, a: the path (/v1/messages), b: the JSON body. The platform adds the host and the credentials.
    ACT_READ,     // id, a: path. The whole file comes back in an EV_FILE (the platform sets a size limit).
    ACT_WRITE,    // id, a: path, b: the new contents. Make the parent directories; replace atomically.
    ACT_SPAWN,    // id, a: command line (run by the shell), n: timeout in ms, flag: 1 = background, c: log path (background)
    ACT_TIMER,    // id, n: milliseconds
    ACT_CANCEL,   // id: stop that HTTP request, process (kill its whole process group), timer or question
    ACT_ASK,      // id, a: the tool, b: what it wants to do. Answer with EV_ANSWER.
    ACT_DONE      // n: the exit status. The platform stops.
} actk_t;

enum { SAY_ASSISTANT, SAY_TOOL, SAY_INFO, SAY_ERROR };

typedef struct {
    actk_t kind;
    int64_t id;
    int flag;
    int64_t n;
    str_t a, b, c;
} act_t;

typedef struct {
    str_t model;
    int64_t max_tokens;      // per reply; 0 for the default
    str_t cwd;               // shown to the model
    str_t platform;          // "Linux", "Windows", ... shown to the model
    str_t date;              // YYYY-MM-DD, shown to the model
    int auto_approve;        // do not ask before writing files or running commands
    int max_turns;           // model calls per user message (a runaway guard); 0 for the default
    int instructions;        // before the first request, read AGENTS.md (or else CLAUDE.md) from the working
                             // directory, with the files its "@file" lines name, into the system prompt
} agent_cfg_t;

typedef struct agent agent_t;

// 0 if memory ran out. The platform keeps `mem` alive as long as the agent.
agent_t* agent_new(const mem_t* mem, const agent_cfg_t* cfg);
void agent_free(agent_t* g);

// Feed one event; get the actions to carry out, in order.
const act_t* agent_step(agent_t* g, const ev_t* ev, size_t* n);

// 1 while the agent is working on a turn (asking the model, running tools, reading the project's
// instructions); 0 when it is waiting for the user, even if background tasks or timers are pending.
// A platform shows its input prompt only when this is 0.
int agent_busy(const agent_t* g);

#endif

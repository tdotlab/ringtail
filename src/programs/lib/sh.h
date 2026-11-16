// The shell language: the core of Saklas, the shell (apps/saklas.c), kept
// apart from the line editor so the host can test it (tests/test_sh.c)
// against dash and bash. It reads POSIX shell: lists, pipelines, && and
// ||, if, while, until, for, case, functions, ( ) and { }, redirections
// and here-documents, variables and parameters, $( ), $(( )), field
// splitting and wildcards. The system underneath is shos.h.
//
// There is no fork. A part of a command that must run apart from the
// shell (a subshell, a pipeline's stage that is not a program, a
// command substitution, something in the background) runs in a new
// shell, `sh -S`, which reads what it needs over descriptor 3: the
// variables, functions and parameters, and the text to run.
#ifndef RINGTAIL_SH_H
#define RINGTAIL_SH_H

// A command the front end adds (saklas: help, history, net and so on).
// fn gets argv[0..argc) and returns an exit status.
typedef struct {
    const char* name;
    int flags;
    int (*fn)(int argc, char** argv);
    const char* args, *help;  // for help
} sh_builtin_t;
// It only writes: it may run inside the shell as the first command of
// a pipeline, rather than in a shell of its own.
#define SHB_PURE 1

typedef struct {
    const sh_builtin_t* builtins;
    int nbuiltins;
    // At the prompt: a command line ended with this status (not 0).
    void (*report_status)(int status);
    // At the prompt: a job in the foreground ended with this signal.
    void (*report_signal)(int sig);
    // Messages ("x: not found") go through here, to be coloured.
    void (*message)(const char* s);
} sh_front_t;

// Runs the shell as argv asks: `sh -c TEXT [NAME ARGS]`, `sh FILE
// [ARGS]`, `sh -S` (a child shell), or commands on standard input.
// Returns the exit status, or SH_INTERACTIVE when standard input is a
// terminal and the front end should run its prompt.
#define SH_INTERACTIVE (-1)
int sh_main(const sh_front_t* front, int argc, char** argv);

// At the prompt. sh_feed runs what text holds (lines ending in '\n').
// SH_MORE: the last command is not finished (an open quote, an if with
// no fi, a here-document still to come); call again with more text
// added. SH_DONE: everything in it has run, or a syntax error was said.
// sh_feed(0) drops an unfinished command (^C at a "> " prompt).
enum { SH_DONE, SH_MORE };
int sh_feed(const char* text);
// After sh_feed: whether `exit` was run, and with which status.
int sh_exit_asked(int* status);
// Set by the front end's SIGINT handler while commands run: stop.
extern volatile int sh_interrupted;
// The core's own commands, for help and Tab.
const sh_builtin_t* sh_core_builtins(int* n);

#endif

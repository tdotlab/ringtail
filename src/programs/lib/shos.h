// What the shell's core (shcore.c) asks of the system it runs on. RingTail
// has one set (shos.c, over the system calls) and the host test another
// (tests/shos_host.c, over POSIX), so the same interpreter runs in both.
//
// Errors come back as a negative errno; RingTail's numbers are Linux's.
#ifndef RINGTAIL_SHOS_H
#define RINGTAIL_SHOS_H

#include <stddef.h>

// How a file is opened for a redirection.
enum {
    SHO_READ,    // <
    SHO_WRITE,   // > and >|: created, emptied
    SHO_APPEND,  // >>: created, written at the end
    SHO_RDWR,    // <>: created, read and written from the start
};
int shos_open(const char* path, int how);
int shos_read(int fd, void* buf, size_t n);        // -EINTR when a signal came first
int shos_write(int fd, const void* buf, size_t n);
int shos_close(int fd);
int shos_dup2(int from, int to);                   // -EBADF when from is not open
int shos_pipe(int fds[2]);
int shos_isatty(int fd);
// What a message says about error e (positive) for path, or another name:
// only the description, which callers put after "what: ".
const char* shos_strerror(const char* what, int e);

// Where the shell keeps a descriptor it has moved out of the way while
// a redirection is in force: SHOS_SAVE_FIRST up to SHOS_SAVE_END - 1.
#define SHOS_SAVE_FIRST 10
#define SHOS_SAVE_END   16

// Whether file names match in either case (RingTail's FAT: 1), for
// wildcards and the order they list in.
extern const int shos_fold_names;

enum { SHS_NONE, SHS_FILE, SHS_DIR };
// SHS_NONE if path is not there; *size gets a file's size.
int shos_stat(const char* path, long* size);
// The next entry of directory path after *cursor (0 to start): 1 with
// its name, 0 at the end, or -errno.
int shos_readdir(const char* path, int* cursor, char* name, size_t size, int* isdir);
int shos_chdir(const char* path);
int shos_getcwd(char* buf, size_t size);
int shos_getpid(void);
int shos_kill(int id, int sig);
__attribute__((noreturn)) void shos_exit(int status);

// Starting a program. fds[0..2] are the caller's descriptors that become
// the child's 0, 1 and 2; -1 asks for it closed where the system can do
// that (RingTail cannot: the child gets the caller's own). fd3, unless it
// is -1, becomes the child's 3. path is
// the program, as typed (a bare name is looked up where programs are),
// or SHOS_SELF for the shell itself. how says which job it goes in:
enum {
    SHJ_SAME,    // the caller's own
    SHJ_NEW_FG,  // a new job, given the terminal
    SHJ_NEW_BG,  // a new job, in the background
    SHJ_JOIN,    // the job `lead` leads
};
#define SHOS_SELF ((const char*)0)
int shos_spawn(const char* path, char* const* argv, const int fds[3], int fd3, int how, int lead);
// Waits for task id to end: its exit status (0-255), with *sig set to
// the signal that ended it or 0. -EINTR if a signal came first.
int shos_wait(int id, int* sig);
// After a job started with SHJ_NEW_FG: waits for everything in it, and
// takes the terminal back.
void shos_job_done(int lead);

#endif

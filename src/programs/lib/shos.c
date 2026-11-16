// shos.h over RingTail's system calls: what the shell's core (shcore.c)
// runs on here. The host test has its own (tests/shos_host.c).
#include "ulib.h"
#include "sh.h"
#include "shos.h"

const int shos_fold_names = 1;  // FAT: README and readme are one file

int shos_open(const char* path, int how) {
    static const int flags[] = { O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC, O_WRONLY | O_CREAT | O_APPEND, O_RDWR | O_CREAT };
    return sys_open(path, flags[how]);
}

int shos_read(int fd, void* buf, size_t n) { return sys_read(fd, buf, (uint32_t)n); }
int shos_write(int fd, const void* buf, size_t n) { return sys_write(fd, buf, (uint32_t)n); }
int shos_close(int fd) { return sys_close(fd); }

int shos_dup2(int from, int to) {
    int r = sys_dup2(from, to);
    return r < 0 ? r : 0;
}

int shos_pipe(int fds[2]) { return sys_pipe(fds); }
int shos_isatty(int fd) { return sys_fdtype(fd) == FDTYPE_TTY; }

const char* shos_strerror(const char* what, int e) {
    if (e == EINVAL && what && *what && !name_ok(what)) return "not a valid file name";
    return strerror(-e);
}

int shos_stat(const char* path, long* size) {
    dirent_t d;
    if (sys_stat(path, &d) < 0) return SHS_NONE;
    *size = (long)d.size;
    return d.flags & DIRENT_DIR ? SHS_DIR : SHS_FILE;
}

int shos_readdir(const char* path, int* cursor, char* name, size_t size, int* isdir) {
    dirent_t d;
    for (;;) {
        int r = sys_readdir(*path ? path : ".", cursor, &d);
        if (r <= 0) return r;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        snprintf(name, size, "%s", d.name);
        *isdir = (d.flags & DIRENT_DIR) != 0;
        return 1;
    }
}

int shos_chdir(const char* path) { return sys_chdir(path); }

int shos_getcwd(char* buf, size_t size) {
    int r = sys_getcwd(buf, (uint32_t)size);
    return r < 0 ? r : 0;
}

int shos_getpid(void) { return sys_getpid(); }
int shos_kill(int id, int sig) { return sys_kill(id, sig); }
void shos_exit(int status) { sys_exit(status); }

int shos_spawn(const char* path, char* const* argv, const int fds[3], int fd3, int how, int lead) {
    static const int flags[] = { 0, SPAWN_NEW_JOB | SPAWN_FOREGROUND, SPAWN_NEW_JOB, SPAWN_JOIN };
    spawn_opts_t o = { flags[how] | (fd3 >= 0 ? SPAWN_FD3 : 0), how == SHJ_JOIN ? lead : 0, { fds[0], fds[1], fds[2] }, fd3 };
    return sys_spawn_opts(path == SHOS_SELF ? "/SAKLAS" : path, (const char* const*)argv, &o);
}

int shos_wait(int id, int* sig) {
    int st = sys_join(id);
    *sig = EXIT_SIGNUM(st);
    if (*sig) return 128 + *sig;
    // A join cut short says -EINTR. Only the front end's Ctrl+C handler
    // can do that here, so a program that exited with -4 is not mistaken
    // for one (the join would say the same again, for ever).
    if (st == -EINTR && sh_interrupted) return -EINTR;
    if (st == -ESRCH) return st;
    return st & 255;
}

void shos_job_done(int lead) {
    sys_wait_job(lead);
    sys_tty_fg(0);
}

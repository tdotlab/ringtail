// shos.h over POSIX, so the shell's core runs on the host (test_sh). A
// child starts with fork and exec; the shell itself is /proc/self/exe.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "shos.h"

const int shos_fold_names = 0;

static int neg(int r) { return r < 0 ? -errno : r; }

int shos_open(const char* path, int how) {
    static const int flags[] = { O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC, O_WRONLY | O_CREAT | O_APPEND, O_RDWR | O_CREAT };
    return neg(open(path, flags[how], 0666));
}

int shos_read(int fd, void* buf, size_t n) { return neg((int)read(fd, buf, n)); }
int shos_write(int fd, const void* buf, size_t n) { return neg((int)write(fd, buf, n)); }
int shos_close(int fd) { return neg(close(fd)); }
int shos_dup2(int from, int to) { return neg(dup2(from, to)); }
int shos_pipe(int fds[2]) { return neg(pipe(fds)); }
int shos_isatty(int fd) { return isatty(fd); }

const char* shos_strerror(const char* what, int e) {
    (void)what;
    switch (e) {  // dash's words, so the cases compare
    case ENOENT: return "No such file or directory";
    case EACCES: return "Permission denied";
    case EISDIR: return "Is a directory";
    }
    return strerror(e);
}

int shos_stat(const char* path, long* size) {
    struct stat st;
    if (stat(path, &st) < 0) return SHS_NONE;
    *size = (long)st.st_size;
    return S_ISDIR(st.st_mode) ? SHS_DIR : SHS_FILE;
}

int shos_readdir(const char* path, int* cursor, char* name, size_t size, int* isdir) {
    DIR* d = opendir(*path ? path : ".");
    if (!d) return -errno;
    struct dirent* e;
    int k = 0, r = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (k++ < *cursor) continue;
        snprintf(name, size, "%s", e->d_name);
        *isdir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            char full[4096];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            *isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        *cursor = k;
        r = 1;
        break;
    }
    closedir(d);
    return r;
}

int shos_chdir(const char* path) { return neg(chdir(path)); }
int shos_getcwd(char* buf, size_t size) { return getcwd(buf, size) ? 0 : -errno; }
int shos_getpid(void) { return getpid(); }
int shos_kill(int id, int sig) { return neg(kill(id, sig)); }
void shos_exit(int status) { _exit(status); }

// Like execvp, without its habit of handing a file that is not a program
// to /bin/sh: ENOEXEC comes back, and the core runs it.
static int exec_path(const char* name, char* const* argv) {
    if (strchr(name, '/')) {
        execv(name, argv);
        return errno;
    }
    const char* path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    int err = ENOENT;
    char full[4096];
    while (*path) {
        const char* c = strchr(path, ':');
        size_t n = c ? (size_t)(c - path) : strlen(path);
        snprintf(full, sizeof full, "%.*s%s%s", (int)n, path, n ? "/" : "", name);
        execv(full, argv);
        if (errno != ENOENT && errno != ENOTDIR) err = errno;
        if (errno == ENOEXEC) return err;
        path += n + (c != 0);
    }
    return err;
}

int shos_spawn(const char* path, char* const* argv, const int fds[3], int fd3, int how, int lead) {
    (void)how;
    (void)lead;  // no job control on the host
    int rep[2];
    if (pipe2(rep, O_CLOEXEC) < 0) return -errno;
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(rep[0]);
        close(rep[1]);
        return -e;
    }
    if (pid == 0) {
        // Move what becomes 0-3 above everything first, so no dup2 clobbers
        // another's source.
        int want[4] = { fds[0], fds[1], fds[2], fd3 }, moved[4];
        for (int i = 0; i < 4; i++) moved[i] = want[i] >= 0 ? fcntl(want[i], F_DUPFD, 64) : -1;
        for (int i = 0; i < 4; i++) {
            if (moved[i] >= 0) dup2(moved[i], i);
            else close(i);  // fds[i] -1: closed
        }
        int keep = rep[1];
        for (int fd = 4; fd < 1024; fd++)
            if (fd != keep) close(fd);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        int e;
        if (path == SHOS_SELF) {
            execv("/proc/self/exe", argv);
            e = errno;
        } else {
            e = exec_path(path, argv);
        }
        (void)!write(keep, &e, sizeof e);
        _exit(127);
    }
    close(rep[1]);
    int e = 0;
    ssize_t r;
    while ((r = read(rep[0], &e, sizeof e)) < 0 && errno == EINTR) {}
    close(rep[0]);
    if (r == (ssize_t)sizeof e) {
        waitpid(pid, 0, 0);
        return -e;
    }
    return pid;
}

int shos_wait(int id, int* sig) {
    int st;
    *sig = 0;
    if (waitpid(id, &st, 0) < 0) return -errno;
    if (WIFSIGNALED(st)) {
        *sig = WTERMSIG(st);
        return 128 + *sig;
    }
    return WEXITSTATUS(st);
}

void shos_job_done(int lead) { (void)lead; }

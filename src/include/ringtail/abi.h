// RingTail system call ABI, shared by the kernel and user programs.
//
// Calling convention: int $0x80 with the call number in eax and up
// to three arguments in ebx, ecx, edx. The result comes back in eax.
// Failures return a negative errno value (-ENOENT and so on).
// Exit statuses are plain ints: a killed program reports EXIT_KILLED
// and one that faulted reports EXIT_FAULTED.
#ifndef RINGTAIL_ABI_H
#define RINGTAIL_ABI_H

#define SYS_PRINT       1   // (buf, len)              -> len
#define SYS_EXIT        2   // (status)                -> does not return
#define SYS_SLEEP       3   // (seconds)               -> 0
#define SYS_OPEN        4   // (name, flags)           -> fd
#define SYS_READ        5   // (fd, buf, len)          -> bytes read, 0 at EOF
#define SYS_WRITE       6   // (fd, buf, len)          -> bytes written
#define SYS_CLOSE       7   // (fd)                    -> 0
#define SYS_DELETE      8   // (path)                  -> 0; files only, rmdir takes directories
#define SYS_GFX_BEGIN   9   // ()                      -> 0, caller owns the display
#define SYS_GFX_END     10  // ()                      -> 0
#define SYS_GFX_PIXEL   11  // (x, y, color)
#define SYS_GFX_CLEAR   12  // (color)
#define SYS_GFX_RECT    13  // (gfx_rect_t*)
#define SYS_GFX_LINE    14  // (gfx_line_t*)
#define SYS_GFX_TEXT    15  // (gfx_text_t*)
#define SYS_MOUSE_STATE 16  // (mouse_state_t*)
#define SYS_KEY_GET     17  // ()                      -> scancode, or -EAGAIN
#define SYS_SEEK        18  // (fd, offset, whence)    -> new offset
#define SYS_LIST        19  // (index, buf, size)      -> name of a file in the current directory, -ENOENT past the end
#define SYS_SPAWN       20  // (name, argv or NULL, spawn_opts_t* or NULL) -> task id; argv is NULL-terminated, argv[0] is the name
#define SYS_WAIT        21  // (last_x, last_y, last_buttons) -> 0 on new input
#define SYS_TICKS       22  // ()                      -> timer ticks since boot (100 Hz)
#define SYS_GETPID      23  // ()                      -> caller's task id
#define SYS_JOIN        24  // (task id)               -> exit status of that task
#define SYS_GFX_BLIT    25  // (const uint8_t* 320x200 pixels) -> 0
#define SYS_GFX_FONT    26  // (uint8_t* 4096 bytes)   -> 0, 256 glyphs of 8x16, one byte per row
#define SYS_SBRK        27  // (increment)             -> old program break, or -ENOMEM
#define SYS_TTY_MODE    28  // (TTY_* flags)           -> previous flags; foreground only
#define SYS_SAVE        29  // (name, buf, len)        -> 0; replaces the file whole or not at all (len <= 1 MB)
#define SYS_TTY_FG      30  // (job)                   -> 0; hand the terminal to a job, or take it back
#define SYS_WAIT_JOB    31  // (job)                   -> 0 once no task of the job is alive
#define SYS_KILL        32  // (task id)               -> 0; kernel tasks cannot be killed
#define SYS_READDIR     33  // (path, int* cursor, dirent_t*) -> 1 with an entry, 0 after the last; *cursor starts at 0
#define SYS_KINFO       34  // (KINFO_*, buf, size)    -> length of the full text; buf gets as much as fits, NUL-terminated
#define SYS_SET_COLOR   35  // (VGA attribute)         -> 0; the console's default colour
#define SYS_POWER       36  // (POWER_*)               -> does not return
#define SYS_PIPE        37  // (int fds[2])            -> 0; fds[0] reads, fds[1] writes
#define SYS_DUP2        38  // (oldfd, newfd)          -> newfd, now a copy of oldfd (closed first if open)
#define SYS_MKDIR       39  // (path)                  -> 0
#define SYS_RMDIR       40  // (path)                  -> 0; the directory must be empty and nobody's current one
#define SYS_CHDIR       41  // (path)                  -> 0
#define SYS_GETCWD      42  // (buf, size)             -> length of the absolute path put in buf
#define SYS_RENAME      43  // (from, to)              -> 0; moves too; to must not exist
#define SYS_STAT        44  // (path, dirent_t*)       -> 0
#define SYS_TIME        45  // (rtc_time_t*)           -> 0; the clock, as the CMOS keeps it
#define SYS_POLL        46  // (ticks)                 -> 1 once terminal input is waiting, 0 if the ticks run out first
#define NUM_SYSCALLS    47

#define EXIT_KILLED  (-1)  // Ctrl+C, Ctrl+backslash or kill
#define EXIT_FAULTED (-2)

// SYS_SPAWN options. By default the child joins the caller's job and
// its descriptors 0, 1 and 2 are copies of the caller's. SPAWN_NEW_JOB
// makes the child the leader of a new job (its id is the job's);
// SPAWN_JOIN puts it in job `job` instead, which must be the caller's
// own or one the caller started. fd[i] >= 0 gives the child a copy of
// the caller's fd[i] as its descriptor i; -1 keeps the default.
#define SPAWN_NEW_JOB 1
#define SPAWN_JOIN    2
typedef struct { int flags, job; int fd[3]; } spawn_opts_t;

// SYS_TTY_FG: the foreground job may hand the terminal to a job it
// started. Once that job has no live tasks, the job that handed it
// over takes it back with SYS_TTY_FG(0), and gets back the terminal
// mode it had.

// SYS_KINFO reports, as the text the shell prints.
#define KINFO_VERSION 0  // the name, version and codename
#define KINFO_MEM     1  // frames and heap, free and total
#define KINFO_HEAP    2  // kernel heap details
#define KINFO_TASKS   3  // the task table
#define KINFO_FAULTS  4  // the fault log
#define KINFO_MMAP    5  // the boot memory map
#define KINFO_DISK    6  // "N KB free of M KB"

#define POWER_REBOOT 0
#define POWER_OFF    1

// Paths: names separated by '/', or "." and "..". One starting with
// '/' begins at the root, any other at the caller's current
// directory, which a spawned program inherits. SYS_SPAWN looks for a
// name without a '/' in the root, where programs live. Names are up
// to NAME_MAX printable ASCII characters except \ / : * ? " < > |,
// not starting with a space or ending in a space or a dot. Case is
// kept but does not matter: README.TXT and readme.txt are one file.
// A long name's short alias (like MYNOTE~1.TXT) names it too.
#define NAME_MAX 255
#define PATH_MAX 256  // including the NUL

// dirent_t.id tells files and directories apart while they exist: two
// paths with the same id are the same thing. A rename can change it.
#define DIRENT_DIR 1  // dirent_t.flags: a directory (size is 0)
// mdate and mtime are when the file was last written, in the FAT's own
// packing: date = (year - 1980) << 9 | month << 5 | day, time = hour << 11 |
// minute << 5 | second / 2. Both are 0 for an entry nothing ever stamped.
typedef struct { char name[NAME_MAX + 1]; unsigned size; unsigned flags; unsigned id; unsigned mdate, mtime; } dirent_t;

// SYS_TIME: the wall clock, in whatever zone the CMOS was set to (UTC
// under QEMU).
typedef struct { unsigned year, month, day, hour, min, sec; } rtc_time_t;

// Values from -4095 to -1 are errors. Anything else is a result,
// including addresses above 2 GB that look negative as an int.
#define IS_ERR(r) ((unsigned)(r) >= (unsigned)-4095)

// Terminal modes for SYS_TTY_MODE. TTY_RAW: reads return keys as
// they arrive, without echo or line editing; Backspace reads as 0x7F
// and the navigation keys as xterm escape sequences (ESC [ A,
// ESC [ 1 ; 5 C for Ctrl+Right, ESC [ 3 ~ for Delete). TTY_NOSIG:
// Ctrl+C is delivered as byte 3 instead of killing the job.
// Ctrl+backslash kills the job in any mode and is never delivered.
// The mode returns to 0 (cooked) when the job leaves the foreground.
#define TTY_RAW   1
#define TTY_NOSIG 2

// Standard descriptors. Every program starts with these open.
#define STDIN_FILENO  0     // console input, foreground only; line at a time unless TTY_RAW
#define STDOUT_FILENO 1     // console output
#define STDERR_FILENO 2     // console output, for errors
#define MAX_FDS 16  // descriptors per program

// open() flags. Values match Linux so they look familiar.
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_ACCMODE 0x0003
#define O_CREAT  0x0040
#define O_EXCL   0x0080     // with O_CREAT: fail with EEXIST if it exists
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define EAGAIN        11
#define ENOMEM        12
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define ENFILE        23
#define EMFILE        24
#define EFBIG         27
#define ENOSPC        28
#define EPIPE         32
#define ENAMETOOLONG  36
#define ENOSYS        38
#define ENOTEMPTY     39

#define GFX_WIDTH  320
#define GFX_HEIGHT 200

// Argument structs for calls that need more than three values.
// All fields are ints, so there is no padding to disagree about.
typedef struct { int x, y, w, h, color, filled; } gfx_rect_t;
typedef struct { int x0, y0, x1, y1, color; } gfx_line_t;
typedef struct { int x, y; const char* str; int color; } gfx_text_t;
typedef struct { int x, y, buttons; } mouse_state_t;

#endif

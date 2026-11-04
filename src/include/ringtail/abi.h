// RingTail system call ABI, shared by the kernel and user programs.
//
// Calling convention: int $0x80 with the call number in eax and up
// to three arguments in ebx, ecx, edx. The result comes back in eax.
// Failures return a negative errno value (-ENOENT and so on).
// Exit statuses are plain ints: a killed program reports -1 and one
// that faulted reports -2.
#ifndef RINGTAIL_ABI_H
#define RINGTAIL_ABI_H

#define SYS_PRINT       1   // (buf, len)              -> len
#define SYS_EXIT        2   // (status)                -> does not return
#define SYS_SLEEP       3   // (seconds)               -> 0
#define SYS_OPEN        4   // (name, flags)           -> fd
#define SYS_READ        5   // (fd, buf, len)          -> bytes read, 0 at EOF
#define SYS_WRITE       6   // (fd, buf, len)          -> bytes written
#define SYS_CLOSE       7   // (fd)                    -> 0
#define SYS_DELETE      8   // (name)                  -> 0
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
#define SYS_LIST        19  // (index, buf, size)      -> 0, -ENOENT past the end
#define SYS_SPAWN       20  // (name, argv or NULL)    -> task id; argv is NULL-terminated, argv[0] is the name
#define SYS_WAIT        21  // (last_x, last_y, last_buttons) -> 0 on new input
#define SYS_TICKS       22  // ()                      -> timer ticks since boot (100 Hz)
#define SYS_GETPID      23  // ()                      -> caller's task id
#define SYS_JOIN        24  // (task id)               -> exit status of that task
#define SYS_GFX_BLIT    25  // (const uint8_t* 320x200 pixels) -> 0
#define SYS_GFX_FONT    26  // (uint8_t* 4096 bytes)   -> 0, 256 glyphs of 8x16, one byte per row
#define SYS_SBRK        27  // (increment)             -> old program break, or -ENOMEM
#define SYS_TTY_MODE    28  // (TTY_* flags)           -> previous flags; foreground only
#define SYS_SAVE        29  // (name, buf, len)        -> 0; replaces the file whole or not at all (len <= 1 MB)
#define NUM_SYSCALLS    30

// Values from -4095 to -1 are errors. Anything else is a result,
// including addresses above 2 GB that look negative as an int.
#define IS_ERR(r) ((unsigned)(r) >= (unsigned)-4095)

// Terminal modes for SYS_TTY_MODE. TTY_RAW: reads return keys as
// they arrive, without echo or line editing; Backspace reads as 0x7F
// and the navigation keys as xterm escape sequences (ESC [ A,
// ESC [ 1 ; 5 C for Ctrl+Right, ESC [ 3 ~ for Delete). TTY_NOSIG:
// Ctrl+C is delivered as byte 3 instead of killing the job; pressing
// it three times within a second still kills it. The mode returns
// to 0 (cooked) when the program's job leaves the foreground.
#define TTY_RAW   1
#define TTY_NOSIG 2

// Standard descriptors. Every program starts with these open.
#define STDIN_FILENO  0     // console input, foreground only; line at a time unless TTY_RAW
#define STDOUT_FILENO 1     // console output

// open() flags. Values match Linux so they look familiar.
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_ACCMODE 0x0003
#define O_CREAT  0x0040
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
#define EINVAL        22
#define EMFILE        24
#define EFBIG         27
#define ENOSPC        28
#define ENAMETOOLONG  36
#define ENOSYS        38

#define GFX_WIDTH  320
#define GFX_HEIGHT 200

// Argument structs for calls that need more than three values.
// All fields are ints, so there is no padding to disagree about.
typedef struct { int x, y, w, h, color, filled; } gfx_rect_t;
typedef struct { int x0, y0, x1, y1, color; } gfx_line_t;
typedef struct { int x, y; const char* str; int color; } gfx_text_t;
typedef struct { int x, y, buttons; } mouse_state_t;

#endif

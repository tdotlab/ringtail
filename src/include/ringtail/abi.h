// RingTail system call ABI, shared by the kernel and user programs.
//
// Calling convention: int $0x80 with the call number in rax and up
// to three arguments in rdi, rsi, rdx. The result comes back in rax.
// Failures return a negative errno value (-ENOENT and so on).
// Exit statuses are plain ints; a program ended by a signal reports
// EXIT_SIGNAL(sig) (see Signals below).
#ifndef RINGTAIL_ABI_H
#define RINGTAIL_ABI_H

#define SYS_PRINT       1   // (buf, len)              -> len
#define SYS_EXIT        2   // (status)                -> does not return
#define SYS_SLEEP       3   // (seconds)               -> 0
#define SYS_OPEN        4   // (name, flags)           -> fd; DEV_TTY is the terminal
#define SYS_READ        5   // (fd, buf, len)          -> bytes read, 0 at EOF
#define SYS_WRITE       6   // (fd, buf, len)          -> bytes written
#define SYS_CLOSE       7   // (fd)                    -> 0
#define SYS_DELETE      8   // (path)                  -> 0; files only, rmdir takes directories
#define SYS_GFX_BEGIN   9   // (GFX_CLASSIC or GFX_NATIVE) -> 0, caller owns the display
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
#define SYS_KILL        32  // (task id, signal)       -> 0; signal 0 only checks the task is there; not kernel tasks
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
#define SYS_PING        47  // (ping_t*)               -> 0 with the reply's rtt_us and ttl filled in
#define SYS_SOCKET      48  // (SOCK_STREAM or SOCK_DGRAM) -> fd of a new TCP or UDP socket
#define SYS_BIND        49  // (fd, port)              -> the port bound; 0 picks a free one
#define SYS_SENDTO      50  // (fd, udp_msg_t*)        -> bytes sent; UDP only
#define SYS_RECVFROM    51  // (fd, udp_msg_t*)        -> bytes received; ip and port say from where; UDP only
#define SYS_NETCONF     52  // (NETCONF_*, netconf_t*) -> 0
#define SYS_LISTEN      53  // (fd, backlog)           -> 0; a TCP socket starts taking connections
#define SYS_ACCEPT      54  // (fd, sock_addr_t* or NULL, timeout_ms) -> fd of the next connection
#define SYS_CONNECT     55  // (fd, const sock_addr_t*, timeout_ms) -> 0 once connected
#define SYS_SHUTDOWN    56  // (fd, SHUT_*)            -> 0
#define SYS_SOCKADDR    57  // (fd, peer, sock_addr_t*) -> 0; the local address, or with peer the remote one
#define SYS_POLLFD      58  // (pollfd_t*, n, timeout_ms) -> how many descriptors have something
#define SYS_FDTYPE      59  // (fd)                    -> FDTYPE_*
#define SYS_RANDOM      60  // (buf, len)              -> len; random bytes fit for keys, 256 at most a call
#define SYS_SETTIME     61  // (const rtc_time_t*)     -> 0; sets the clock (see below)
#define SYS_SIGACTION   62  // (signal, const sigaction_t* or NULL, sigaction_t* old or NULL) -> 0
#define SYS_SIGMASK     63  // (SIG_BLOCK, SIG_UNBLOCK or SIG_SETMASK, mask) -> the mask before
#define SYS_SIGRETURN   64  // () -> does not return: a handler's way back (sigaction_t.restorer)
#define SYS_GFX_INFO    65  // (gfx_info_t*)           -> 0; the size native graphics draws at
#define SYS_GFX_PUT     66  // (const gfx_put_t*)      -> 0; native only: pixels to the screen
#define SYS_TTY_SIZE    67  // (fd, tty_size_t*)       -> 0, or -ENOTTY if fd is not the terminal
#define NUM_SYSCALLS    68

// Signals, numbered as on Linux. Each one's default action is to end
// the program, which then reports EXIT_SIGNAL(sig). A program can
// ignore a signal or catch it with a handler instead (SYS_SIGACTION),
// or block it for a while (SYS_SIGMASK), and it stays pending until
// unblocked; all but SIGKILL, which always ends it. A signal that is
// pending and not blocked interrupts a blocking call, which returns
// -EINTR (after the handler, if there is one, has run).
//
// Where they come from: SYS_KILL (kill in the shell, SIGTERM unless
// told otherwise); Ctrl+C sends SIGINT to the foreground job, unless it
// asked for TTY_NOSIG, and Ctrl+backslash SIGQUIT in any mode (a third
// within two seconds sends SIGKILL, so a program that catches both
// cannot lock the terminal); writing to a pipe with no reader sends
// SIGPIPE (and fails with EPIPE if it is ignored or caught); and a
// fault sends the signal named for it: SIGSEGV for a bad memory access,
// SIGILL, SIGFPE, SIGBUS or SIGTRAP. A fault the program does not catch
// is reported on the screen and in the fault log. If a handler returns
// from a fault, the instruction runs again.
//
// Spawned programs start with every signal at its default and none
// blocked.
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define NSIG     16  // signals are 1 to NSIG - 1
#define SIGBIT(sig) (1u << (sig))

#define EXIT_SIGNAL(sig) (-256 - (sig))
// The signal that ended a program with this exit status, or 0.
#define EXIT_SIGNUM(st)  ((st) < -256 && (st) > -256 - NSIG ? -256 - (st) : 0)

// SYS_SIGACTION. A handler runs on the program's own stack as
// handler(sig), with the signals in mask blocked and, unless
// SA_NODEFER, sig itself; it returns to restorer, which must call
// SYS_SIGRETURN to go back to where the program was, with its
// registers and mask as they were. ulib's sigaction fills restorer in.
// SA_RESETHAND puts the default back as the handler is called.
// Interrupted calls are not restarted.
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SA_RESETHAND 1
#define SA_NODEFER   2
typedef struct { sighandler_t handler; unsigned mask; unsigned flags; void (*restorer)(void); } sigaction_t;

#define SIG_BLOCK   0  // SYS_SIGMASK: add these to the mask
#define SIG_UNBLOCK 1  // take these out
#define SIG_SETMASK 2  // the mask becomes these

// SYS_SPAWN options. By default the child joins the caller's job and
// its descriptors 0, 1 and 2 are copies of the caller's. SPAWN_NEW_JOB
// makes the child the leader of a new job (its id is the job's);
// SPAWN_JOIN puts it in job `job` instead, which must be the caller's
// own or one the caller started. fd[i] >= 0 gives the child a copy of
// the caller's fd[i] as its descriptor i; -1 keeps the default.
// SPAWN_FOREGROUND hands the terminal to the child's job, as SYS_TTY_FG
// would, before the child runs at all, so it never reads as a
// background job; only the terminal's current owner may use it.
// SPAWN_FD3 also gives the child a copy of the caller's fd3 as its
// descriptor 3 (the shell hands a child shell its variables that way);
// without it the child has nothing open past 2.
#define SPAWN_NEW_JOB    1
#define SPAWN_JOIN       2
#define SPAWN_FOREGROUND 4
#define SPAWN_FD3        8
typedef struct { int flags, job; int fd[3]; int fd3; } spawn_opts_t;
#define SPAWN_FDS 4  // descriptors a child can be handed: 0-2 and 3

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
#define KINFO_NET     7  // the network card, the address and the packet counts
#define KINFO_PCI     8  // every PCI device: bus:dev.fn vendor:device class irq
#define KINFO_CLUSTER 9  // the bytes in a cluster of the disk, as one number
#define KINFO_CPUS    10 // each processor: its APIC ID, ticks, task switches, the task it runs

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
// under QEMU). SYS_SETTIME sets it, to the second: -EINVAL for a date
// or time that does not exist, -ERANGE for one before the kernel was
// built (so a lying time server cannot wind the clock back to when an
// expired certificate was good) or after 2099 (the chip keeps two digits
// of year). The chip starts counting half a second after the call: to
// set it exactly, call when the time is t and a half.
typedef struct { unsigned year, month, day, hour, min, sec; } rtc_time_t;

// Values from -4095 to -1 are errors. Anything else is a result,
// including addresses above 2 GB that look negative as an int.
#define IS_ERR(r) ((unsigned long)(r) >= (unsigned long)-4095)

// Terminal modes for SYS_TTY_MODE. TTY_RAW: reads return keys as
// they arrive, without echo or line editing; Backspace reads as 0x7F
// and the navigation keys as xterm escape sequences (ESC [ A,
// ESC [ 1 ; 5 C for Ctrl+Right, ESC [ 3 ~ for Delete). TTY_NOSIG:
// Ctrl+C is delivered as byte 3 instead of sending SIGINT.
// Ctrl+backslash sends SIGQUIT in any mode and is never delivered.
// The mode returns to 0 (cooked) when the job leaves the foreground.
#define TTY_RAW   1
#define TTY_NOSIG 2

// Standard descriptors. Every program starts with these open.
// open(DEV_TTY, O_RDONLY) is the keyboard and open(DEV_TTY, O_WRONLY)
// the screen, whatever the standard descriptors point at, so a program
// at the end of a pipe can still ask the person at the terminal a
// question. It behaves as standard input does: foreground jobs only,
// and in the terminal's current mode. It is not a file and hides any
// file of that name; O_RDWR and the creating flags are refused.
#define DEV_TTY "/dev/tty"

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
#define ENOTTY        25
#define EFBIG         27
#define ENOSPC        28
#define EPIPE         32
#define ERANGE        34
#define ENAMETOOLONG  36
#define ENOSYS        38
#define ENOTEMPTY     39
#define ENOTSOCK      88
#define EMSGSIZE      90
#define EOPNOTSUPP    95
#define EADDRINUSE    98
#define EADDRNOTAVAIL 99
#define ENETDOWN      100
#define ENETUNREACH   101
#define ECONNABORTED  103
#define ECONNRESET    104
#define EISCONN       106
#define ENOTCONN      107
#define ETIMEDOUT     110
#define ECONNREFUSED  111
#define EHOSTUNREACH  113
#define EALREADY      114

// Graphics comes in two modes, chosen by SYS_GFX_BEGIN. GFX_CLASSIC is
// 320x200 in 256 colours, drawn with SYS_GFX_PIXEL and the rest; colours
// 0-15 are the text colours, 16-31 greys from black to white, and 32-247
// a 6x6x6 cube (32 + 36*r + 6*g + b, each 0-5). GFX_NATIVE is the
// screen's own size (SYS_GFX_INFO) in 24-bit colour: a program draws
// into memory of its own and shows rectangles of it with SYS_GFX_PUT.
// Pixels there are 0xRRGGBB; the top byte is ignored. The mouse is in
// the mode's own pixels.
#define GFX_CLASSIC 0
#define GFX_NATIVE  1
#define GFX_WIDTH  320
#define GFX_HEIGHT 200

// Argument structs for calls that need more than three values.
// All fields are ints, so there is no padding to disagree about.
typedef struct { int x, y, w, h, color, filled; } gfx_rect_t;
typedef struct { int x0, y0, x1, y1, color; } gfx_line_t;
typedef struct { int x, y; const char* str; int color; } gfx_text_t;
typedef struct { int x, y, buttons; } mouse_state_t;
typedef struct { int width, height; } gfx_info_t;
// w x h pixels to x, y; stride is how many pixels apart rows are in
// `pixels`. What falls off the screen is dropped.
typedef struct { int x, y, w, h, stride, pad; const unsigned* pixels; } gfx_put_t;
// The terminal's size in characters.
typedef struct { int cols, rows; } tty_size_t;

// SYS_PING: send one ICMP echo request to ip (host order, so 10.0.2.2
// is 0x0A000202) carrying `size` bytes of data (0 to PING_MAX_SIZE),
// and wait up to timeout_ms (at most 10 seconds) for the reply. On
// success rtt_us is the round trip in microseconds and ttl the reply's
// time to live. Errors: -ENETDOWN with no network card or no link,
// -EHOSTUNREACH when nobody answers ARP for the next hop (or a router
// says so), -ETIMEDOUT when no reply came, -EIO for a reply whose data
// came back changed, -EINTR if the caller was killed while waiting.
#define PING_MAX_SIZE 1472
typedef struct { unsigned ip; int seq, size, timeout_ms; int rtt_us, ttl; } ping_t;

// UDP sockets. A socket is a descriptor like a file's: close() frees
// it, and spawned programs and dup2 share it. SYS_BIND gives it a port
// (1 to 65535, or 0 for a free one from 49152 up) and returns the port;
// SYS_SENDTO binds an unbound socket to a free port first. Port 68 is
// the kernel's DHCP client's.
//
// SYS_SENDTO sends len bytes (0 to UDP_MAX_SIZE) from buf to ip:port;
// ip 255.255.255.255 broadcasts on the local network. SYS_RECVFROM
// takes the oldest datagram waiting, copying at most len bytes of it to
// buf (the rest is lost) and filling in ip and port with its sender.
// It waits timeout_ms for one: 0 returns -EAGAIN at once when none is
// there, and a negative timeout waits for ever. Datagrams to 127.0.0.1
// or to the machine's own address go round inside it. A socket holds
// at most 16 datagrams waiting; more are dropped.
//
// Errors: -ENOTSOCK for a descriptor that is not a socket, -EADDRINUSE
// for a port someone else has, -EMSGSIZE, -ENETDOWN without a card or
// a link, -EADDRNOTAVAIL before the machine has an address, and
// -EHOSTUNREACH or -ENETUNREACH as for ping; -ETIMEDOUT when the wait
// runs out, -EINTR if the caller was killed while waiting.
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define UDP_MAX_SIZE 1472
typedef struct { unsigned ip; int port; void* buf; int len; int timeout_ms; } udp_msg_t;

// TCP sockets: SYS_SOCKET(SOCK_STREAM). A socket either connects
// (SYS_CONNECT) or listens (SYS_BIND for its port, then SYS_LISTEN)
// and hands out each connection that comes as a new descriptor
// (SYS_ACCEPT). A connection is read and written with SYS_READ and
// SYS_WRITE: a read waits for at least one byte and returns 0 once the
// other end has finished sending; a write waits until all of it is
// taken (16 KB are buffered each way). close() finishes sending what
// is buffered in the background; closing with data unread resets the
// connection instead.
//
// SYS_CONNECT waits for the answer: timeout_ms > 0 gives up sooner
// than TCP itself would (31 s), anything else waits for TCP. A socket
// whose connect failed may try again. SYS_ACCEPT waits timeout_ms for
// a connection: 0 returns -EAGAIN at once, negative waits for ever;
// with addr it fills in the peer. backlog (1 to 16) is how many
// connections may wait to be accepted. SYS_SHUTDOWN stops reading
// (SHUT_RD), sending (SHUT_WR: the other end reads the end) or both.
//
// Errors: -ECONNREFUSED (nobody listens there), -ETIMEDOUT (no answer,
// or the peer stopped answering), -ECONNRESET (the peer reset it),
// -ECONNABORTED (the machine's address went away), -ENOTCONN, -EISCONN,
// -EALREADY (a connect is under way), -EPIPE (writing after
// SHUT_WR), -EOPNOTSUPP (a UDP call on a TCP socket, or the other way
// round), and those of UDP. An error is reported once, by the next
// call; after it the connection reads as ended.
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2
typedef struct { unsigned ip; int port; } sock_addr_t;

// SYS_POLLFD waits up to timeout_ms (0: not at all, negative: for
// ever) until at least one of the n (at most MAX_FDS) descriptors has
// one of the events it asks for, and sets revents in each. POLLERR,
// POLLHUP and POLLNVAL (fd not open) are reported whether asked for or
// not; a negative fd is skipped. The terminal is readable once a key
// is waiting for a program in the foreground (in cooked mode a read
// still waits for the whole line). POLLOUT on a TCP connection means
// a write of 4 KB would not wait.
#define POLLIN   1
#define POLLOUT  4
#define POLLERR  8
#define POLLHUP  16
#define POLLNVAL 32
typedef struct { int fd, events, revents; } pollfd_t;

// SYS_FDTYPE: what a descriptor is.
#define FDTYPE_TTY  1  // the terminal, either way
#define FDTYPE_FILE 2
#define FDTYPE_PIPE 3
#define FDTYPE_UDP  4
#define FDTYPE_TCP  5

// SYS_RANDOM: bytes nobody can predict, for keys and anything else an
// attacker must not guess. They come from the kernel's generator, which
// feeds on interrupt timing (and the CPU's own generator where it has
// one). Never blocks. At most RANDOM_MAX bytes a call (-EINVAL beyond).
#define RANDOM_MAX 256

// SYS_NETCONF. NETCONF_GET fills in the configuration. NETCONF_STATIC
// sets ip, mask, gw and dns by hand (gw and dns may be 0 for none) and
// stops the DHCP client; NETCONF_DHCP drops the address and starts it.
// mode says which of the two is in charge; with DHCP, ip is 0 until a
// server has answered, lease is the seconds left (-1 for no end) and
// server the DHCP server. domain is the DNS domain a server gave, for
// trying names without a dot in it ("printer" as printer.lan). ntp
// is the time servers a DHCP server named (option 42), 0 for none;
// NETCONF_STATIC ignores it and there are none with an address set by
// hand.
#define NETCONF_GET    0
#define NETCONF_STATIC 1
#define NETCONF_DHCP   2
typedef struct {
    unsigned ip, mask, gw, dns[2];
    int mode;  // NETCONF_STATIC or NETCONF_DHCP
    int lease;
    unsigned server;
    char domain[64];
    unsigned ntp[2];
} netconf_t;

#endif

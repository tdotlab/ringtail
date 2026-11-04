// System calls: int 0x80, number in eax, arguments in ebx, ecx, edx,
// result in eax (negative errno on failure). Interrupts are on while
// a call runs, so calls can block.
//
// Every pointer a program passes is a user pointer and goes through
// the checked copy routines in paging.c. Nothing here dereferences
// one directly except gfx_blit, after user_range_ok has proven the
// whole buffer mapped and user readable.
#include "kernel.h"
#include "interrupt.h"
#include "task.h"
#include "fs.h"
#include "mm.h"
#include "tty.h"
#include "console.h"
#include "drivers.h"
#include "arch.h"
#include "ringtail/abi.h"

typedef int (*syscall_fn)(uint32_t a, uint32_t b, uint32_t c);

static int sys_print(uint32_t ubuf, uint32_t len, uint32_t c) {
    (void)c;
    if (user_range_ok(ubuf, len, 0)) return -EFAULT;
    char buf[256];
    uint32_t done = 0;
    console_lock();  // one call's output comes out in one piece
    while (done < len) {
        uint32_t n = MIN(len - done, sizeof buf);
        if (copy_from_user(buf, (const char*)(ubuf + done), n)) break;
        console_write(buf, n);
        done += n;
    }
    console_unlock();
    return (int)done;
}

static int sys_exit(uint32_t status, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    task_exit((int)status);
}

static int sys_sleep(uint32_t seconds, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (seconds > 86400) seconds = 86400;
    return task_sleep_ticks(seconds * TIMER_HZ);
}

static int sys_open(uint32_t name, uint32_t flags, uint32_t c) {
    (void)c;
    return file_open((const char*)name, (int)flags);
}

static int sys_read(uint32_t fd, uint32_t buf, uint32_t len) {
    return file_read((int)fd, (void*)buf, len);
}

static int sys_write(uint32_t fd, uint32_t buf, uint32_t len) {
    return file_write((int)fd, (const void*)buf, len);
}

static int sys_close(uint32_t fd, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return file_close((int)fd);
}

static int copy_name(char name[FS_NAME_MAX], uint32_t uname) {
    int n = strncpy_from_user(name, (const char*)uname, FS_NAME_MAX);
    return n == -ENAMETOOLONG ? -EINVAL : n;
}

static int sys_delete(uint32_t uname, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    char name[FS_NAME_MAX];
    int n = copy_name(name, uname);
    return n < 0 ? n : diskfs_unlink(name);
}

// ---- Graphics: only the task that owns the display may draw ----

static int is_owner(void) { return gfx_owner() == current->id; }

static int sys_gfx_begin(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    if (is_owner()) return 0;
    return gfx_acquire(current->id);
}

static int sys_gfx_end(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return gfx_release(current->id);
}

static int sys_gfx_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!is_owner()) return -EPERM;
    gfx_pixel((int)x, (int)y, (uint8_t)color);
    return 0;
}

static int sys_gfx_clear(uint32_t color, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_clear((uint8_t)color);
    return 0;
}

static int sys_gfx_rect(uint32_t uarg, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_rect_t r;
    if (copy_from_user(&r, (const void*)uarg, sizeof r)) return -EFAULT;
    gfx_rect(r.x, r.y, r.w, r.h, (uint8_t)r.color, r.filled);
    return 0;
}

static int sys_gfx_line(uint32_t uarg, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_line_t l;
    if (copy_from_user(&l, (const void*)uarg, sizeof l)) return -EFAULT;
    gfx_line(l.x0, l.y0, l.x1, l.y1, (uint8_t)l.color);
    return 0;
}

static int sys_gfx_text(uint32_t uarg, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    gfx_text_t t;
    if (copy_from_user(&t, (const void*)uarg, sizeof t)) return -EFAULT;
    char s[GFX_WIDTH / 8 + 1];  // more would be off screen anyway
    int n = strncpy_from_user(s, t.str, sizeof s);
    if (n == -ENAMETOOLONG) {
        s[sizeof s - 1] = 0;  // the copy filled the buffer: show what fits
    } else if (n < 0) {
        return n;
    }
    gfx_text(t.x, t.y, s, (uint8_t)t.color);
    return 0;
}

static int sys_gfx_blit(uint32_t upix, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    if (user_range_ok(upix, GFX_WIDTH * GFX_HEIGHT, 0)) return -EFAULT;
    gfx_blit((const uint8_t*)upix);
    return 0;
}

static int sys_gfx_font(uint32_t ubuf, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return copy_to_user((void*)ubuf, gfx_font(), 256 * 16);
}

// ---- Input ----

static int sys_mouse_state(uint32_t uarg, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    mouse_state_t m;
    mouse_get(&m.x, &m.y, &m.buttons);
    return copy_to_user((void*)uarg, &m, sizeof m);
}

static int sys_key_get(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    if (!is_owner()) return -EPERM;
    int k = keyboard_raw_get();
    return k < 0 ? -EAGAIN : k;
}

// Something new for the caller: the mouse differs from what it last
// saw, or a key is waiting for it.
static int input_ready(int lx, int ly, int lb) {
    int x, y, btn;
    mouse_get(&x, &y, &btn);
    if (x != lx || y != ly || btn != lb) return 1;
    if (is_owner()) return keyboard_raw_pending();
    return current->job == tty_foreground() && tty_pending();
}

static int sys_wait(uint32_t lx, uint32_t ly, uint32_t lb) {
    uint32_t f = irq_save();
    int r = 0;
    while (!input_ready((int)lx, (int)ly, (int)lb)) {
        if (current->killed) { r = -EINTR; break; }
        task_block(&input_chan);
    }
    irq_restore(f);
    return r;
}

// ---- Files and tasks ----

static int sys_seek(uint32_t fd, uint32_t off, uint32_t whence) {
    return file_seek((int)fd, (int32_t)off, (int)whence);
}

// Name of the index-th file. Restarts from the top on every call:
// slow for a big directory, but stateless.
static int sys_list(uint32_t index, uint32_t ubuf, uint32_t size) {
    char name[FS_NAME_MAX];
    int cursor = 0;
    for (uint32_t i = 0;; i++) {
        if (diskfs_readdir(&cursor, name, 0) < 0) return -ENOENT;
        if (i == index) break;
    }
    uint32_t len = strlen(name) + 1;
    if (size < len) return -EINVAL;
    return copy_to_user((void*)ubuf, name, len);
}

// argv from user memory: up to ARGV_MAX strings, ARGS_MAX_BYTES in
// all, NULL-terminated. A NULL argv means just the program name.
#define SPAWN_ARGS_BYTES 2048

static int sys_spawn(uint32_t uname, uint32_t uargv, uint32_t c) {
    (void)c;
    char name[FS_NAME_MAX];
    int n = copy_name(name, uname);
    if (n < 0) return n;
    // While the caller holds the display the child could not print
    // anything visible, and could not take the display either.
    if (is_owner()) return -EBUSY;
    if (!uargv) {
        const char* argv[1] = { name };
        return loader_spawn(name, 1, argv, current->job);
    }
    char* strings = kmalloc(SPAWN_ARGS_BYTES);
    if (!strings) return -ENOMEM;
    const char* argv[ARGV_MAX];
    int argc = 0, r = 0;
    uint32_t used = 0;
    for (;;) {
        uint32_t up;
        if (copy_from_user(&up, (const void*)(uargv + (uint32_t)argc * 4), 4)) { r = -EFAULT; break; }
        if (!up) break;
        if (argc == ARGV_MAX) { r = -E2BIG; break; }
        int len = strncpy_from_user(strings + used, (const char*)up, SPAWN_ARGS_BYTES - used);
        if (len == -ENAMETOOLONG) { r = -E2BIG; break; }
        if (len < 0) { r = len; break; }
        argv[argc++] = strings + used;
        used += (uint32_t)len + 1;
    }
    if (!r && argc == 0) r = -EINVAL;
    if (!r) r = loader_spawn(name, argc, argv, current->job);
    kfree(strings);
    return r;
}

// Move the program break. The pages between the old and new break
// are mapped at once, so the heap never takes a page fault. Shrinking
// moves the break but keeps the pages, which a later grow reuses.
static int sys_sbrk(uint32_t incr, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    uint32_t old = current->brk;
    int64_t want = (int64_t)old + (int32_t)incr;
    if (want < (int64_t)current->brk_start || want > (int64_t)USER_IMAGE_LIMIT) return -ENOMEM;
    uint32_t nb = (uint32_t)want;
    for (uint32_t va = ALIGN_UP(old, PAGE_SIZE); va < ALIGN_UP(nb, PAGE_SIZE); va += PAGE_SIZE)
        if (!as_map(current->cr3, va, 1)) return -ENOMEM;
    current->brk = nb;
    return (int)old;
}

static int sys_tty_mode(uint32_t mode, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return tty_set_mode((int)mode);
}

static int sys_ticks(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return (int)timer_ticks();
}

static int sys_getpid(uint32_t a, uint32_t b, uint32_t c) {
    (void)a; (void)b; (void)c;
    return current->id;
}

static int sys_join(uint32_t id, uint32_t b, uint32_t c) {
    (void)b; (void)c;
    return task_join((int)id);
}

static const syscall_fn table[NUM_SYSCALLS] = {
    [SYS_PRINT] = sys_print,
    [SYS_EXIT] = sys_exit,
    [SYS_SLEEP] = sys_sleep,
    [SYS_OPEN] = sys_open,
    [SYS_READ] = sys_read,
    [SYS_WRITE] = sys_write,
    [SYS_CLOSE] = sys_close,
    [SYS_DELETE] = sys_delete,
    [SYS_GFX_BEGIN] = sys_gfx_begin,
    [SYS_GFX_END] = sys_gfx_end,
    [SYS_GFX_PIXEL] = sys_gfx_pixel,
    [SYS_GFX_CLEAR] = sys_gfx_clear,
    [SYS_GFX_RECT] = sys_gfx_rect,
    [SYS_GFX_LINE] = sys_gfx_line,
    [SYS_GFX_TEXT] = sys_gfx_text,
    [SYS_MOUSE_STATE] = sys_mouse_state,
    [SYS_KEY_GET] = sys_key_get,
    [SYS_SEEK] = sys_seek,
    [SYS_LIST] = sys_list,
    [SYS_SPAWN] = sys_spawn,
    [SYS_WAIT] = sys_wait,
    [SYS_TICKS] = sys_ticks,
    [SYS_GETPID] = sys_getpid,
    [SYS_JOIN] = sys_join,
    [SYS_GFX_BLIT] = sys_gfx_blit,
    [SYS_GFX_FONT] = sys_gfx_font,
    [SYS_SBRK] = sys_sbrk,
    [SYS_TTY_MODE] = sys_tty_mode,
};

void syscall_handle(regs_t* r) {
    uint32_t n = r->eax;
    if (n >= NUM_SYSCALLS || !table[n]) {
        r->eax = (uint32_t)-ENOSYS;
        return;
    }
    r->eax = (uint32_t)table[n](r->ebx, r->ecx, r->edx);
}

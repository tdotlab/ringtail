// v0.6.0: syscalls, the doorway between programs and the kernel.
// A program in ring 3 cannot do privileged things directly: cli,
// outb, and friends fault there. What it CAN do is int 0x80, which
// traps into ring 0 through the DPL 3 gate in idt.c, and the
// handler here decides what the kernel will do on its behalf.
// Calling convention, kept deliberately boring: eax = syscall
// number, ebx = first argument, ecx = second, edx = third, and
// the result comes back in eax. The first three syscalls were
// print bytes, sleep seconds, and exit, because that is all
// hello and count needed. v0.7.2 adds file syscalls, so programs
// can use the disk the shell already uses: open, read, write,
// close, delete, and, since v0.10.1, seek. Everything else a program wants
// (formatting numbers, building strings) it does itself, in its
// own code, on its own stack: the kernel provides primitives,
// not a library. That split is the point, and it is why the
// count program carries its own number-to-text code instead of
// calling a kernel print_num.
// File syscall design, in one paragraph. Descriptors are
// per-task slot numbers, 0-7, held in the task struct (see
// task_fd_open and friends in task.c): open takes a filename and
// returns a slot, read and write take a slot, a user buffer, and
// a length, close frees a slot, delete takes a filename, seek
// moves a slot's offset (from the start, from the current
// offset, or from the end of the file). There are no modes and
// no permissions in this slice: every open is read/write, every
// program can open any file, because there are no users yet to
// give permissions to. Open creates the file if it does not
// exist, like the shell's write auto-create, and resets the
// offset to 0; read and write advance the offset, so a large
// file is handled in chunks, and seek lets a program jump
// back, skip ahead, or read the tail without reopening.
// Read and write lengths are capped at 4096, like print, so a
// single syscall never copies an unbounded user buffer. Errors
// are all -1, with no errno: a program that needs to know why
// must print its own diagnostic, which is the toy tradeoff for
// not inventing an error-number table yet.
// Two honesty notes, updated for v0.7.2. First, print runs under
// the print lock, so a program's output cannot interleave
// mid-line with the counter or the shell, and because
// syscall_isr re-enables interrupts, a program spinning on that
// lock gets preempted and the holder runs to release it, instead
// of deadlocking with the timer off. File syscalls do not take
// the print lock, but they do take the disk filesystem's own
// interrupt lock per disk operation, through diskfs, so a timer
// switch cannot catch a bitmap or directory update half done.
// Second, pointer validation is bounded, not full: names are
// copied from user space with a 32-byte bound (a name with no
// null in 32 bytes is rejected, so the kernel never reads an
// unbounded user string), and read/write lengths are capped at
// 4096. A pointer to an unmapped page still faults, but since
// v0.7.1 that fault kills only the calling program (see
// badptr), so shallow validation is now survivable, not
// OS-killing. Concurrent writes to the same file from two
// programs are not atomic in this slice: each write reads the
// whole file, modifies it, and writes it back, and another task
// can interleave between the read and the write, so the last
// writer can win with stale content. Programs should not share
// a file concurrently yet; file locking is a later piece.

#include <stdint.h>

#define SYS_PRINT  1
#define SYS_EXIT   2
#define SYS_SLEEP  3
#define SYS_OPEN   4
#define SYS_READ   5
#define SYS_WRITE  6
#define SYS_CLOSE  7
#define SYS_DELETE 8
#define SYS_GFX_BEGIN 9
#define SYS_GFX_END 10
#define SYS_GFX_PIXEL 11
#define SYS_GFX_CLEAR 12
#define SYS_GFX_RECT 13
#define SYS_GFX_LINE 14
#define SYS_GFX_TEXT 15
#define SYS_MOUSE_STATE 16
#define SYS_KEY_GET 17
#define SYS_SEEK 18
#define SYS_LIST 19
#define SYS_SPAWN 20

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern void print_char(char c);
extern void vga_lock(void);
extern void vga_unlock(void);
extern int task_sleep(uint32_t seconds);
extern void task_exit(void);
extern int task_current_id(void);
extern int keyboard_poll(void);
extern int mouse_get_x(void);
extern int mouse_get_y(void);
extern int mouse_get_buttons(void);
extern void gfx_graphics_begin(void);
extern void gfx_graphics_end(void);
extern void gfx_pixel(int x, int y, uint8_t color);
extern void gfx_clear(uint8_t color);
extern void gfx_rect(int x, int y, int w, int h, uint8_t color, int filled);
extern void gfx_line(int x0, int y0, int x1, int y1, uint8_t color);
extern void gfx_text(int x, int y, const char* s, uint8_t color);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern int task_fd_open(const char* name);
extern int task_fd_close(int fd);
extern int task_fd_lookup(int fd, char* name_out, uint32_t* offset_out);
extern int task_fd_advance(int fd, uint32_t new_offset);
extern int diskfs_create(const char* name);
extern int diskfs_delete(const char* name);
extern int diskfs_write_bytes(const char* name, const uint8_t* content, uint32_t len);
extern uint8_t* diskfs_read_all(const char* name, uint32_t* size_out);
extern int diskfs_size(const char* name, uint32_t* size_out);
extern int diskfs_list_at(int index, char* name_out);
extern int loader_run(const char* name);

// Copy a null-terminated filename from user space, with a hard
// 32-byte bound. Returns 0 on success (kernel_buf holds the
// name, null-terminated), -1 on null pointer, empty name, or no
// null within 32 bytes. Reading user memory byte by byte is safe
// here because a fault on an unmapped byte kills only the caller
// (v0.7.1 recovery), and the bound means at most 32 bytes are
// ever read, even from a hostile or buggy program.
static int copy_user_name(const char* user_ptr, char* kernel_buf) {
    if (!user_ptr) return -1;
    for (int i = 0; i < 32; i++) {
        char ch = user_ptr[i];
        kernel_buf[i] = ch;
        if (ch == 0) {
            if (i == 0) return -1; // empty name
            return 0;
        }
    }
    return -1; // no null in 32 bytes: too long
}

// Graphics ownership, v0.9.0. Only one program may own graphics
// mode at a time, because there is one framebuffer and one saved
// text screen: two owners would overwrite each other's save and
// restore garbage. The owner is a task id, or -1 when nobody
// owns it. BEGIN by the owner again is a no-op success (it must
// NOT re-save, or the saved text screen would be overwritten
// with the graphics state it is supposed to restore to). BEGIN
// by anyone else fails. Drawing calls and KEY_GET fail for
// non-owners. MOUSE_STATE is read-only and is allowed for any
// task. If the owner dies without calling END (it faults, or it
// forgets), task_cleanup calls gfx_task_exited, which restores
// text mode, so the OS never stays stuck in graphics mode with
// no program drawing and no visible shell.
static volatile int gfx_owner = -1;

int gfx_is_active(void) { return gfx_owner != -1; }

void gfx_task_exited(int id) {
    if (id == gfx_owner) {
        gfx_graphics_end();
        gfx_owner = -1;
    }
}

uint32_t syscall_dispatch(uint32_t num, uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    if (num == SYS_PRINT) {
        const char* s = (const char*)a;
        uint32_t len = b;
        if (!s || len > 4096) return (uint32_t)-1;
        vga_lock();
        for (uint32_t i = 0; i < len; i++) print_char(s[i]);
        vga_unlock();
        return len;
    }
    if (num == SYS_SLEEP) {
        int r = task_sleep(a);
        return (uint32_t)r;
    }
    if (num == SYS_EXIT) {
        task_exit();
        // Never reached: task_exit switches away from a dead
        // task, which is never picked again.
        return 0;
    }
    if (num == SYS_OPEN) {
        char name[32];
        if (copy_user_name((const char*)a, name) != 0) return (uint32_t)-1;
        // Create if missing, like the shell's write auto-create:
        // diskfs_create returns 0 (created) or -1 (already there),
        // both of which mean the file exists now and can be opened.
        int cr = diskfs_create(name);
        if (cr != 0 && cr != -1) return (uint32_t)-1;
        int fd = task_fd_open(name);
        if (fd < 0) return (uint32_t)-1;
        return (uint32_t)fd;
    }
    if (num == SYS_CLOSE) {
        int r = task_fd_close((int)a);
        return (uint32_t)r;
    }
    if (num == SYS_READ) {
        int fd = (int)a;
        uint8_t* user_buf = (uint8_t*)b;
        uint32_t len = c;
        if (!user_buf || len > 4096) return (uint32_t)-1;
        if (len == 0) return 0;
        char name[32];
        uint32_t offset = 0;
        if (task_fd_lookup(fd, name, &offset) != 0) return (uint32_t)-1;
        uint32_t size = 0;
        uint8_t* file_buf = diskfs_read_all(name, &size);
        if (!file_buf) return (uint32_t)-1; // missing, too big, or OOM
        if (offset >= size) {
            kfree(file_buf);
            return 0; // end of file
        }
        uint32_t to_copy = size - offset;
        if (to_copy > len) to_copy = len;
        for (uint32_t i = 0; i < to_copy; i++) user_buf[i] = file_buf[offset + i];
        kfree(file_buf);
        task_fd_advance(fd, offset + to_copy);
        return to_copy;
    }
    if (num == SYS_WRITE) {
        int fd = (int)a;
        const uint8_t* user_buf = (const uint8_t*)b;
        uint32_t len = c;
        if (!user_buf || len > 4096) return (uint32_t)-1;
        if (len == 0) return 0;
        char name[32];
        uint32_t offset = 0;
        if (task_fd_lookup(fd, name, &offset) != 0) return (uint32_t)-1;
        uint32_t old_size = 0;
        uint8_t* old_buf = diskfs_read_all(name, &old_size);
        if (!old_buf) return (uint32_t)-1; // deleted after open, too big, or OOM
        uint32_t new_size = old_size;
        if (offset + len > new_size) new_size = offset + len;
        uint8_t* new_buf = (uint8_t*)kmalloc(new_size);
        if (!new_buf) {
            kfree(old_buf);
            return (uint32_t)-1;
        }
        for (uint32_t i = 0; i < new_size; i++) new_buf[i] = 0;
        for (uint32_t i = 0; i < old_size; i++) new_buf[i] = old_buf[i];
        for (uint32_t i = 0; i < len; i++) new_buf[offset + i] = user_buf[i];
        kfree(old_buf);
        int wr = diskfs_write_bytes(name, new_buf, new_size);
        kfree(new_buf);
        if (wr != 0) return (uint32_t)-1;
        task_fd_advance(fd, offset + len);
        return len;
    }
    if (num == SYS_DELETE) {
        char name[32];
        if (copy_user_name((const char*)a, name) != 0) return (uint32_t)-1;
        int r = diskfs_delete(name);
        if (r != 0) return (uint32_t)-1;
        return 0;
    }
    if (num == SYS_GFX_BEGIN) {
        int me = task_current_id();
        if (gfx_owner == me) return 0; // already owner: no re-save
        if (gfx_owner != -1) return (uint32_t)-1; // owned by another
        gfx_graphics_begin();
        gfx_owner = me;
        return 0;
    }
    if (num == SYS_GFX_END) {
        int me = task_current_id();
        if (gfx_owner != me) return (uint32_t)-1;
        gfx_graphics_end();
        gfx_owner = -1;
        return 0;
    }
    // Drawing calls: owner only, or a text program could draw
    // over the desktop (or into a text-mode framebuffer window
    // that is not displayed, corrupting video memory state).
    // The check is one comparison; the primitives themselves
    // still clip, so a wild coordinate cannot write past the
    // framebuffer even for the owner.
    if (num == SYS_GFX_PIXEL) {
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        gfx_pixel((int)a, (int)b, (uint8_t)c);
        return 0;
    }
    if (num == SYS_GFX_CLEAR) {
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        gfx_clear((uint8_t)a);
        return 0;
    }
    if (num == SYS_GFX_RECT) {
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        const int* r = (const int*)a;
        if (!r) return (uint32_t)-1;
        gfx_rect(r[0], r[1], r[2], r[3], (uint8_t)r[4], r[5]);
        return 0;
    }
    if (num == SYS_GFX_LINE) {
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        const int* l = (const int*)a;
        if (!l) return (uint32_t)-1;
        gfx_line(l[0], l[1], l[2], l[3], (uint8_t)l[4]);
        return 0;
    }
    if (num == SYS_GFX_TEXT) {
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        const int* t = (const int*)a;
        if (!t) return (uint32_t)-1;
        const char* user_str = (const char*)t[2];
        if (!user_str) return (uint32_t)-1;
        // Copy to a kernel buffer, bounded: a user string with
        // no null in 256 bytes is truncated, not read forever,
        // and an unmapped pointer faults and kills only the
        // caller, like every other user buffer in this file.
        char kbuf[256];
        int i;
        for (i = 0; i < 255; i++) {
            char ch = user_str[i];
            kbuf[i] = ch;
            if (ch == 0) break;
        }
        kbuf[i] = 0;
        gfx_text(t[0], t[1], kbuf, (uint8_t)t[3]);
        return 0;
    }
    if (num == SYS_MOUSE_STATE) {
        // Read-only, allowed for any task: the mouse position is
        // a sensor reading, not framebuffer state, and the shell
        // already exposes it through the `mouse` command.
        int* m = (int*)a;
        if (!m) return (uint32_t)-1;
        m[0] = mouse_get_x();
        m[1] = mouse_get_y();
        m[2] = mouse_get_buttons();
        return 0;
    }
    if (num == SYS_KEY_GET) {
        // Owner only, for the queue reason in the stub header:
        // the main loop stops draining while graphics is owned,
        // so the owner is the only drainer, and two drainers
        // cannot race on the queue tail.
        if (task_current_id() != gfx_owner) return (uint32_t)-1;
        int sc = keyboard_poll();
        return (uint32_t)sc; // -1 if empty, else the scancode
    }
    if (num == SYS_SEEK) {
        // Seek: move an open file's read/write offset, like Unix
        // lseek. a = fd, b = offset (signed), c = whence
        // (0 = from start, 1 = from current, 2 = from end).
        // Returns the new absolute offset, or -1 on failure,
        // with the offset unchanged on failure. SET and CUR
        // only do offset arithmetic, so they do not touch the
        // disk and succeed even if the file was deleted after
        // opening; END needs the file's size, so it fails if
        // the file is gone or there is no disk. Seeking past
        // end of file is allowed: a later read returns 0
        // (end of file), and a later write extends the file
        // with a zero-filled gap, by the existing write logic.
        // The new offset must fit in a signed 32-bit int
        // (0..2147483647), because the program-side wrapper
        // returns int and -1 means error; a larger offset
        // would be indistinguishable from an error.
        int fd = (int)a;
        int32_t off = (int32_t)b;
        int whence = (int)c;
        char name[32];
        uint32_t cur = 0;
        if (task_fd_lookup(fd, name, &cur) != 0) return (uint32_t)-1;
        int64_t base;
        if (whence == SEEK_SET) {
            base = 0;
        } else if (whence == SEEK_CUR) {
            base = (int64_t)cur;
        } else if (whence == SEEK_END) {
            uint32_t size = 0;
            if (diskfs_size(name, &size) != 0) return (uint32_t)-1;
            base = (int64_t)size;
        } else {
            return (uint32_t)-1;
        }
        int64_t new_off = base + (int64_t)off;
        if (new_off < 0 || new_off > 2147483647LL) return (uint32_t)-1;
        if (task_fd_advance(fd, (uint32_t)new_off) != 0) return (uint32_t)-1;
        return (uint32_t)new_off;
    }
    if (num == SYS_LIST) {
        // List: a = index, b = user buffer pointer, c = buffer
        // size in bytes. Returns 0 on success with the Nth
        // regular file's display name copied into the buffer,
        // null-terminated, or -1 on failure (index out of
        // range, null buffer, buffer too small, no disk).
        // Call with increasing index, starting at 0, until
        // -1 to enumerate every file on disk. The order and
        // the skip rules match diskfs_list, so LIST and the
        // shell's ls agree on what is on disk. The size check
        // happens before any copy, so a too-small buffer
        // gets -1 and no partial name, never a truncation.
        // A negative buffer size, passed as a signed int,
        // is rejected the same way: the check is signed.
        int idx = (int)a;
        char* user_buf = (char*)b;
        int32_t max_len = (int32_t)c;
        if (!user_buf || max_len <= 0) return (uint32_t)-1;
        char kname[16];
        if (diskfs_list_at(idx, kname) != 0) return (uint32_t)-1;
        uint32_t len = 0;
        while (kname[len]) len++;
        if (len + 1 > (uint32_t)max_len) return (uint32_t)-1;
        for (uint32_t i = 0; i <= len; i++) user_buf[i] = kname[i];
        return 0;
    }
    if (num == SYS_SPAWN) {
        // Spawn: a = filename pointer. Returns the new task
        // id on success, or -1 on failure (no such file,
        // load failed, no free task slot, bad name). It
        // reuses loader_run, like the shell's run command,
        // so a spawned program is a normal task, not a
        // special kind of task. One guard: it refuses while
        // the caller owns graphics mode, because the new
        // program's output would be invisible. Text output
        // goes to the text screen, which graphics mode
        // hides, and the restore on graphics exit would
        // overwrite it, so the output would be lost, not
        // just delayed. A caller in graphics mode must
        // GFX_END first, then spawn, then exit.
        if (task_current_id() == gfx_owner) return (uint32_t)-1;
        char name[32];
        if (copy_user_name((const char*)a, name) != 0) return (uint32_t)-1;
        int id = loader_run(name);
        return (uint32_t)id;
    }
    return (uint32_t)-1; // unknown syscall number
}

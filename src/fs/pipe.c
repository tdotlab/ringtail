// Pipes: a 4 KB ring buffer with a reading end and a writing end.
// Each end counts the descriptors that refer to it, across tasks.
// A read waits for data and returns 0 once no writer is left; a
// write waits for room and fails with EPIPE once no reader is left.
// Both ends wait on the pipe itself, and every change wakes both
// sides; wakeups may be spurious, so each side re-checks.
#include "fs.h"
#include "task.h"
#include "arch.h"
#include "mm.h"
#include "lib.h"
#include "ringtail/abi.h"

#define PIPE_SIZE 4096u

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t head, len;  // data starts at head and wraps
    int readers, writers;
};

// Every change wakes both ends, and anyone polling.
static void changed(struct pipe* p) {
    task_wakeup(p);
    poll_notify();
}

struct pipe* pipe_new(void) {
    struct pipe* p = kmalloc(sizeof *p);
    if (!p) return 0;
    memset(p, 0, sizeof *p);
    p->readers = p->writers = 1;
    return p;
}

void pipe_ref(struct pipe* p, int writer) {
    uint32_t f = irq_save();
    if (writer) p->writers++;
    else p->readers++;
    irq_restore(f);
}

void pipe_unref(struct pipe* p, int writer) {
    uint32_t f = irq_save();
    if (writer) p->writers--;
    else p->readers--;
    int gone = p->readers == 0 && p->writers == 0;
    changed(p);  // a reader may now see EOF, a writer EPIPE
    irq_restore(f);
    if (gone) kfree(p);
}

int pipe_read(struct pipe* p, uint8_t* kbuf, uint32_t len) {
    uint32_t f = irq_save();
    int r;
    for (;;) {
        if (p->len) {
            uint32_t n = MIN(len, p->len);
            for (uint32_t i = 0; i < n; i++) kbuf[i] = p->buf[(p->head + i) % PIPE_SIZE];
            p->head = (p->head + n) % PIPE_SIZE;
            p->len -= n;
            changed(p);
            r = (int)n;
            break;
        }
        if (p->writers == 0) { r = 0; break; }
        if (current->killed) { r = -EINTR; break; }
        task_block(p);
    }
    irq_restore(f);
    return r;
}

int pipe_write(struct pipe* p, const uint8_t* kbuf, uint32_t len) {
    uint32_t f = irq_save();
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        if (p->readers == 0) { err = -EPIPE; break; }
        uint32_t room = PIPE_SIZE - p->len;
        if (room) {
            uint32_t n = MIN(len - done, room);
            uint32_t tail = p->head + p->len;
            for (uint32_t i = 0; i < n; i++) p->buf[(tail + i) % PIPE_SIZE] = kbuf[done + i];
            p->len += n;
            done += n;
            changed(p);
            continue;
        }
        if (current->killed) { err = -EINTR; break; }
        task_block(p);
    }
    irq_restore(f);
    return done ? (int)done : err;
}

int pipe_events(struct pipe* p, int writer) {
    uint32_t f = irq_save();
    int ev;
    if (writer) ev = p->readers == 0 ? POLLERR : p->len < PIPE_SIZE ? POLLOUT : 0;
    else ev = (p->len ? POLLIN : 0) | (p->writers == 0 ? POLLIN | POLLHUP : 0);
    irq_restore(f);
    return ev;
}

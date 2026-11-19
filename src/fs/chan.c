// Channels (abi.h): two-way message pipes between tasks, found by name.
//
// A channel has two sides, 0 for the task that connected and 1 for the
// one that accepted, and a queue of messages for each side to read.
// Each side is one open file (dup and inheritance share it), so a side
// closes once, when that open file goes. The channel is freed when both
// have closed. A listener holds the channels connected to it until they
// are accepted; side 1 of those is open already, so the connecting task
// can write before it is accepted, and closing the listener closes it.
//
// Everything here runs under the big kernel lock with interrupts off
// around each look-then-sleep, as in pipe.c. Every change wakes the
// whole channel or listener, and anyone polling; woken tasks re-check.
#include "fs.h"
#include "task.h"
#include "arch.h"
#include "mm.h"
#include "lib.h"
#include "drivers.h"
#include "ringtail/abi.h"

typedef struct {
    uint16_t len;
    uint8_t data[CHAN_MSG_MAX];
} msg_t;

typedef struct {
    msg_t m[CHAN_QUEUE];
    uint32_t head, count;
} queue_t;

struct chan {
    queue_t q[2];  // q[s]: messages for side s to read
    int open[2];
};

struct chan_listener {
    char name[CHAN_NAME_MAX + 1];
    struct chan* waiting[CHAN_BACKLOG];  // oldest first
    int nwaiting;
    struct chan_listener* next;
};

static struct chan_listener* listeners;

static void changed(void* what) {
    task_wakeup(what);
    poll_notify();
}

static struct chan_listener* find(const char* name) {
    for (struct chan_listener* l = listeners; l; l = l->next)
        if (!strcmp(l->name, name)) return l;
    return 0;
}

int chan_name_ok(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n > CHAN_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/') return 0;
    return 1;
}

int chan_listen(const char* name, struct chan_listener** out) {
    if (!chan_name_ok(name)) return -EINVAL;
    if (find(name)) return -EADDRINUSE;
    struct chan_listener* l = kzalloc(sizeof *l);
    if (!l) return -ENOMEM;
    memcpy(l->name, name, strlen(name) + 1);
    l->next = listeners;
    listeners = l;
    *out = l;
    return 0;
}

void chan_close(struct chan* c, int side) {
    uint32_t f = irq_save();
    c->open[side] = 0;
    int gone = !c->open[0] && !c->open[1];
    changed(c);  // the other side may now see the end, or EPIPE
    irq_restore(f);
    if (gone) kfree(c);
}

void chan_unlisten(struct chan_listener* l) {
    struct chan_listener** pp = &listeners;
    while (*pp != l) pp = &(*pp)->next;
    *pp = l->next;
    for (int i = 0; i < l->nwaiting; i++) chan_close(l->waiting[i], 1);
    changed(l);
    kfree(l);
}

int chan_connect(const char* name, struct chan** out) {
    if (!chan_name_ok(name)) return -EINVAL;
    struct chan_listener* l = find(name);
    if (!l || l->nwaiting == CHAN_BACKLOG) return -ECONNREFUSED;
    struct chan* c = kzalloc(sizeof *c);
    if (!c) return -ENOMEM;
    c->open[0] = c->open[1] = 1;
    uint32_t f = irq_save();
    l->waiting[l->nwaiting++] = c;
    changed(l);
    irq_restore(f);
    *out = c;
    return 0;
}

int chan_accept(struct chan_listener* l, struct chan** out, int timeout_ms) {
    uint64_t ticks = timeout_ms > 0 ? ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000 : 0;
    uint32_t until = timer_ticks() + (uint32_t)ticks;
    uint32_t f = irq_save();
    int r;
    for (;;) {
        if (l->nwaiting) {
            *out = l->waiting[0];
            l->nwaiting--;
            memmove(l->waiting, l->waiting + 1, (size_t)l->nwaiting * sizeof l->waiting[0]);
            r = 0;
            break;
        }
        if (timeout_ms == 0) { r = -EAGAIN; break; }
        if (task_signalled(current)) { r = -EINTR; break; }
        if (timeout_ms > 0 && (int32_t)(timer_ticks() - until) >= 0) { r = -ETIMEDOUT; break; }
        if (timeout_ms < 0) task_block(l);
        else task_block_timeout(l, until);
    }
    irq_restore(f);
    return r;
}

int chan_read(struct chan* c, int side, uint8_t* kbuf, uint32_t len) {
    queue_t* q = &c->q[side];
    uint32_t f = irq_save();
    int r;
    for (;;) {
        if (q->count) {
            const msg_t* m = &q->m[q->head];
            uint32_t n = MIN(len, m->len);
            memcpy(kbuf, m->data, n);
            q->head = (q->head + 1) % CHAN_QUEUE;
            q->count--;
            changed(c);
            r = (int)n;
            break;
        }
        if (!c->open[!side]) { r = 0; break; }
        if (task_signalled(current)) { r = -EINTR; break; }
        task_block(c);
    }
    irq_restore(f);
    return r;
}

int chan_write(struct chan* c, int side, const uint8_t* kbuf, uint32_t len) {
    if (len > CHAN_MSG_MAX) return -EMSGSIZE;
    queue_t* q = &c->q[!side];
    uint32_t f = irq_save();
    int r;
    for (;;) {
        if (!c->open[!side]) { r = -EPIPE; break; }
        if (q->count < CHAN_QUEUE) {
            msg_t* m = &q->m[(q->head + q->count) % CHAN_QUEUE];
            m->len = (uint16_t)len;
            memcpy(m->data, kbuf, len);
            q->count++;
            changed(c);
            r = (int)len;
            break;
        }
        if (task_signalled(current)) { r = -EINTR; break; }
        task_block(c);
    }
    irq_restore(f);
    return r;
}

int chan_events(struct chan* c, int side) {
    uint32_t f = irq_save();
    int ev;
    if (!c->open[!side]) ev = POLLIN | POLLHUP;
    else ev = (c->q[side].count ? POLLIN : 0) | (c->q[!side].count < CHAN_QUEUE ? POLLOUT : 0);
    irq_restore(f);
    return ev;
}

int chan_listener_events(struct chan_listener* l) { return l->nwaiting ? POLLIN : 0; }

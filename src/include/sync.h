// Sleeping locks. A mutex blocks the waiting task instead of
// spinning, so the holder can run and release it. Recursive: the
// owner may lock it again. Never use one from an interrupt handler.
#ifndef RINGTAIL_SYNC_H
#define RINGTAIL_SYNC_H

struct task;

typedef struct {
    volatile int locked;
    struct task* owner;
    int depth;
} mutex_t;

#define MUTEX_INIT { 0, 0, 0 }

void mutex_lock(mutex_t* m);
void mutex_unlock(mutex_t* m);

#endif

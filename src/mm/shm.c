// Shared memory (SYS_SHM_NEW, SYS_SHM_MAP, SYS_SHM_UNMAP in abi.h).
//
// An object is a set of frames with an id. Every task that maps it
// holds one reference per mapping, kept in the task's shm table; the
// frames go back with the last reference, so memory a program made and
// handed to another lives until both are done with it, whichever goes
// first. The page table entries carry PTE_SHARED (paging.c), so tearing
// down an address space never frees an object's frames by mistake.
//
// The big kernel lock is held throughout and nothing here blocks, so
// the object list needs no lock of its own.
#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "ringtail/abi.h"

typedef struct shm {
    uint32_t id;
    uint32_t pages;
    int refs;           // mappings, in every task
    uintptr_t* frames;  // pages of them
    struct shm* next;
} shm_t;

static shm_t* objects;
static uint32_t next_id = 1;

static shm_t* find(uint32_t id) {
    for (shm_t* o = objects; o; o = o->next)
        if (o->id == id) return o;
    return 0;
}

static void put(shm_t* o) {
    if (--o->refs > 0) return;
    shm_t** pp = &objects;
    while (*pp != o) pp = &(*pp)->next;
    *pp = o->next;
    for (uint32_t i = 0; i < o->pages; i++) frame_free(o->frames[i]);
    kfree(o->frames);
    kfree(o);
}

// Free addresses for len bytes in t's shared region, with an unmapped
// page after each mapping so running off the end of one faults instead
// of landing in the next. 0 when there is no room.
static uintptr_t find_room(task_t* t, uintptr_t len) {
    uintptr_t cand = SHM_BASE;
    for (;;) {
        if (cand + len > USER_SHM_TOP || cand + len < cand) return 0;
        int moved = 0;
        for (int i = 0; i < SHM_PER_TASK; i++) {
            const struct shm_map* m = &t->shm[i];
            if (!m->obj) continue;
            uintptr_t end = m->va + (uintptr_t)m->obj->pages * PAGE_SIZE + PAGE_SIZE;
            if (cand < end && m->va < cand + len + PAGE_SIZE) {
                cand = end;
                moved = 1;
            }
        }
        if (!moved) return cand;
    }
}

static long map_into(task_t* t, shm_t* o, shm_info_t* info) {
    int slot = -1;
    for (int i = 0; i < SHM_PER_TASK && slot < 0; i++)
        if (!t->shm[i].obj) slot = i;
    if (slot < 0) return -EMFILE;
    uintptr_t len = (uintptr_t)o->pages * PAGE_SIZE;
    uintptr_t va = find_room(t, len);
    if (!va) return -ENOMEM;
    for (uint32_t i = 0; i < o->pages; i++) {
        int r = as_share(t->cr3, va + (uintptr_t)i * PAGE_SIZE, o->frames[i]);
        if (r) {
            while (i--) as_unshare(t->cr3, va + (uintptr_t)i * PAGE_SIZE);
            return r;
        }
    }
    o->refs++;
    t->shm[slot].obj = o;
    t->shm[slot].va = va;
    info->id = o->id;
    info->size = o->pages * PAGE_SIZE;
    info->addr = (void*)va;
    return 0;
}

long shm_new(uint32_t size, shm_info_t* info) {
    if (size == 0 || size > SHM_MAX) return -EINVAL;
    shm_t* o = kzalloc(sizeof *o);
    if (!o) return -ENOMEM;
    o->pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    o->frames = kzalloc(o->pages * sizeof *o->frames);
    if (!o->frames) { kfree(o); return -ENOMEM; }
    for (uint32_t i = 0; i < o->pages; i++) {
        o->frames[i] = frame_alloc();  // zeroed
        if (!o->frames[i]) {
            while (i--) frame_free(o->frames[i]);
            kfree(o->frames);
            kfree(o);
            return -ENOMEM;
        }
    }
    o->id = next_id++;
    o->next = objects;
    objects = o;
    long r = map_into(current, o, info);
    if (r) {
        o->refs = 1;  // the list's, so put frees it
        put(o);
        return r;
    }
    return o->id;
}

long shm_map(uint32_t id, shm_info_t* info) {
    shm_t* o = find(id);
    if (!o) return -ENOENT;
    return map_into(current, o, info);
}

static void unmap_slot(task_t* t, struct shm_map* m) {
    shm_t* o = m->obj;
    for (uint32_t i = 0; i < o->pages; i++) as_unshare(t->cr3, m->va + (uintptr_t)i * PAGE_SIZE);
    m->obj = 0;
    m->va = 0;
    put(o);
}

long shm_unmap(uintptr_t addr) {
    task_t* t = current;
    for (int i = 0; i < SHM_PER_TASK; i++)
        if (t->shm[i].obj && t->shm[i].va == addr) {
            unmap_slot(t, &t->shm[i]);
            return 0;
        }
    return -EINVAL;
}

void shm_task_exit(task_t* t) {
    for (int i = 0; i < SHM_PER_TASK; i++)
        if (t->shm[i].obj) unmap_slot(t, &t->shm[i]);
}

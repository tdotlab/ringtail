// Shared memory: making it, mapping it twice, the bad arguments, the
// per-task limit, frames given back, and memory passed to a child by
// its id. Children (this program again, with a role) are for what has
// to happen in another task, or ends one.
#include "ulib.h"

static int frames_free_kb(void) {
    char text[256];
    sys_kinfo(KINFO_MEM, text, sizeof text);
    const char* h = text;
    while (*h && strncmp(h, "Frames:", 7)) h++;
    if (!*h) return -1;
    h += 7;
    while (*h == ' ') h++;
    uint32_t v = 0;
    while (*h >= '0' && *h <= '9') v = v * 10 + (uint32_t)(*h++ - '0');
    return (int)v;
}

static int spawn_role(const char* role, const char* arg, int out) {
    const char* argv[] = { "shmtest", role, arg, 0 };
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { -1, out, -1 }, -1 };
    return sys_spawn_opts("shmtest", argv, &o);
}

static int child(const char* role, const char* arg) {
    uint32_t id = 0;
    if (arg) parse_uint(arg, &id);
    shm_info_t in;
    if (!strcmp(role, "peek")) {  // map id, check the pattern, answer in it
        if (sys_shm_map(id, &in) != 0) return 2;
        uint8_t* p = in.addr;
        for (unsigned i = 0; i < in.size; i++)
            if (p[i] != (uint8_t)(i * 7)) return 3;
        memcpy(p, "child was here", 15);
        return 0;
    }
    if (!strcmp(role, "hold")) {  // map id, then wait for EOF on stdin
        if (sys_shm_map(id, &in) != 0) return 2;
        sys_write(STDOUT_FILENO, "held\n", 5);
        char c;
        while (sys_read(STDIN_FILENO, &c, 1) > 0) {}
        return ((uint8_t*)in.addr)[0] == 0x5A ? 0 : 3;
    }
    if (!strcmp(role, "after")) {  // touch memory just unmapped
        if (sys_shm_new(4096, &in) < 0) return 2;
        ((volatile uint8_t*)in.addr)[0] = 1;
        sys_shm_unmap((uintptr_t)in.addr);
        return ((volatile uint8_t*)in.addr)[0];  // faults
    }
    if (!strcmp(role, "keep")) {  // exit holding a mapping
        if (sys_shm_new(256 * 1024, &in) < 0) return 2;
        memset(in.addr, 1, in.size);
        return 0;
    }
    return 99;
}

int main(int argc, char** argv) {
    if (argc > 1) return child(argv[1], argc > 2 ? argv[2] : 0);

    shm_info_t a, b, c;
    check(sys_shm_new(0, &a) == -EINVAL, "an empty object is refused");
    check(sys_shm_new(SHM_MAX + 1, &a) == -EINVAL, "so is one over the limit");
    check(SC2(SYS_SHM_NEW, 0x100000000ull + 4096, &a) == -EINVAL, "and one whose size only fits in 64 bits");

    int id = sys_shm_new(5000, &a);
    check(id > 0 && a.id == (unsigned)id, "make an object");
    check(a.size == 8192, "its size is rounded up to whole pages");
    uintptr_t va = (uintptr_t)a.addr;
    check(va >= SHM_BASE && va < 0xC0000000u && !(va & 4095), "it is mapped page-aligned in the shared region");
    int zero = 1;
    for (unsigned i = 0; i < a.size; i++) zero &= ((uint8_t*)a.addr)[i] == 0;
    check(zero, "and starts zeroed");

    check(sys_shm_map((unsigned)id, &b) == 0 && b.id == a.id && b.size == a.size, "map it a second time");
    check(b.addr != a.addr && (uintptr_t)b.addr >= va + a.size + 4096, "at its own address, after an unmapped page");
    ((uint8_t*)a.addr)[100] = 0x42;
    ((uint8_t*)b.addr)[8000] = 0x43;
    check(((uint8_t*)b.addr)[100] == 0x42 && ((uint8_t*)a.addr)[8000] == 0x43, "both views are the same memory");

    check(sys_shm_unmap(va + 4096) == -EINVAL, "unmapping needs the start address");
    check(sys_shm_unmap(va) == 0, "unmap the first view");
    check(sys_shm_unmap(va) == -EINVAL, "not twice");
    check(sys_shm_unmap(0) == -EINVAL && sys_shm_unmap(0xFFFFFFFFFFFFF000ull) == -EINVAL, "nor what was never mapped");
    check(((uint8_t*)b.addr)[100] == 0x42, "the other view keeps the data");
    check(sys_shm_map((unsigned)id, &c) == 0 && c.addr == a.addr, "a freed range is used again");
    sys_shm_unmap((uintptr_t)c.addr);

    check(sys_shm_map(0, &c) == -ENOENT && sys_shm_map(999999, &c) == -ENOENT, "unknown ids give ENOENT");
    check(SC2(SYS_SHM_MAP, 0x100000000ull + (unsigned)id, &c) == -ENOENT, "and ids are not cut to 32 bits");
    check(sys_shm_new(4096, 0) == -EFAULT && sys_shm_new(4096, (shm_info_t*)0xC0000000ull) == -EFAULT,
          "a bad info pointer gives EFAULT");
    check(sys_shm_map((unsigned)id, (shm_info_t*)1) == -EFAULT, "for map too");
    sys_shm_unmap((uintptr_t)b.addr);
    check(sys_shm_map((unsigned)id, &c) == -ENOENT, "the last unmap ends the object");

    // The table holds SHM_PER_TASK mappings, and the failures above
    // left nothing in it.
    shm_info_t many[SHM_PER_TASK + 1];
    int made = 0, r;
    while ((r = sys_shm_new(4096, &many[made])) > 0) made++;
    check(r == -EMFILE && made == SHM_PER_TASK, "a task holds at most SHM_PER_TASK mappings");
    for (int i = 0; i < made; i++) sys_shm_unmap((uintptr_t)many[i].addr);

    // Frames: taken by an object, back when it ends.
    int before = frames_free_kb();
    id = sys_shm_new(SHM_MAX, &a);
    int during = frames_free_kb();
    check(id > 0 && during <= before - (int)(SHM_MAX / 1024), "the largest object takes its frames");
    ((uint8_t*)a.addr)[0] = 1;
    ((uint8_t*)a.addr)[SHM_MAX - 1] = 2;
    check(((uint8_t*)a.addr)[SHM_MAX - 1] == 2, "and every page of it works");
    sys_shm_unmap((uintptr_t)a.addr);
    int after = frames_free_kb();
    // less the page tables that mapped it (2 MB a table), which stay
    // with the address space
    check(after >= before - (int)((SHM_MAX >> 21) + 1) * 4, "they come back on unmap");

    // A child maps the object by its id and writes into it.
    id = sys_shm_new(3 * 4096, &a);
    for (unsigned i = 0; i < a.size; i++) ((uint8_t*)a.addr)[i] = (uint8_t)(i * 7);
    char ids[12];
    snprintf(ids, sizeof ids, "%d", id);
    int k = spawn_role("peek", ids, -1);
    check(k > 0 && sys_join(k) == 0, "a child maps it by id and sees what was written");
    check(!memcmp(a.addr, "child was here", 15), "and what it writes shows up here");
    sys_shm_unmap((uintptr_t)a.addr);

    // The object lives while anyone has it mapped, whoever made it.
    id = sys_shm_new(4096, &a);
    ((uint8_t*)a.addr)[0] = 0x5A;
    snprintf(ids, sizeof ids, "%d", id);
    int in[2], out[2];
    sys_pipe(in);
    sys_pipe(out);
    const char* hold[] = { "shmtest", "hold", ids, 0 };
    spawn_opts_t o = { SPAWN_NEW_JOB, 0, { in[0], out[1], -1 }, -1 };
    k = sys_spawn_opts("shmtest", hold, &o);
    sys_close(in[0]);
    sys_close(out[1]);
    char buf[8];
    int got = sys_read(out[0], buf, sizeof buf);
    sys_shm_unmap((uintptr_t)a.addr);
    check(got == 5 && sys_shm_map((unsigned)id, &b) == 0 && ((uint8_t*)b.addr)[0] == 0x5A,
          "an object outlives its maker's mapping while a child holds it");
    sys_shm_unmap((uintptr_t)b.addr);
    sys_close(in[1]);
    check(k > 0 && sys_join(k) == 0, "and the child still sees it");
    sys_close(out[0]);
    check(sys_shm_map((unsigned)id, &b) == -ENOENT, "it ends with the child");

    // Mappings go with the task that held them.
    before = frames_free_kb();
    k = spawn_role("keep", 0, -1);
    check(k > 0 && sys_join(k) == 0, "a child exits holding memory");
    sys_sleep(1);  // long enough for it to be reaped
    check(frames_free_kb() >= before - 16, "and its frames come back");

    k = spawn_role("after", 0, -1);
    check(k > 0 && sys_join(k) == EXIT_SIGNAL(SIGSEGV), "a child touching memory it unmapped dies of SIGSEGV");

    return check_summary("shmtest");
}

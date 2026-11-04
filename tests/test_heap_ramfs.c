// RingTail host tests - heap and RAMFS, v0.3.0
//
// Runs the real kernel heap.c and ramfs.c on the host, in 32-bit
// mode, so the pointer-width assumptions in the kernel code hold
// exactly as they do on the target. Build and run with:
//   make run
// in this directory (needs gcc -m32, i.e. gcc-multilib on Ubuntu).
//
// Not covered here, on purpose: timer.c and keyboard.c use x86
// port I/O, which faults in user mode, so they are QEMU-only.
// The kernel main loop and shell are also QEMU-only.
//
// The heap under test is a dedicated static array: heap_init
// starts the heap at _kernel_end, page-aligned, and the array is
// 8 KB larger than the heap, so alignment slack cannot spill
// into other globals.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Heap storage: heap.c declares extern char _kernel_end[] and
// builds a 4 MB heap starting at its page-aligned address.
char _kernel_end[4 * 1024 * 1024 + 8192];

// Stubs for the kernel print functions ramfs_list uses.
void print(const char* s) { fputs(s, stdout); }
void print_char(char c) { putchar(c); }

extern void heap_init(void);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern uint32_t heap_get_used(void);
extern uint32_t heap_get_free(void);
extern uint32_t heap_get_free_blocks(void);
extern int ramfs_create(const char* name);
extern int ramfs_write(const char* name, const char* content);
extern int ramfs_read(const char* name, char* out, uint32_t max);
extern int ramfs_delete(const char* name);
extern void ramfs_list(void);

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    heap_init();
    CHECK(heap_get_used() == 0, "heap starts empty");
    CHECK(heap_get_free_blocks() == 1, "heap starts as one free block");
    CHECK(heap_get_free() == 4 * 1024 * 1024 - 16, "initial free size is heap minus header");

    // Basic alloc/free and coalescing.
    void* p = kmalloc(64);
    CHECK(p != NULL, "malloc 64 succeeds");
    CHECK(((uintptr_t)p % 8) == 0, "malloc returns 8-byte aligned pointer");
    CHECK(heap_get_used() > 0, "heap used grows after malloc");
    CHECK(kfree(p) == 0, "free of a live block succeeds");
    CHECK(heap_get_used() == 0, "heap used returns to 0 after free");
    CHECK(heap_get_free_blocks() == 1, "free coalesces back to one block");

    // Zero and overflow rejections (the v0.3.0 allocator fix).
    CHECK(kmalloc(0) == NULL, "malloc 0 returns NULL");
    CHECK(kmalloc(0xFFFFFFFFu) == NULL, "malloc 4294967295 returns NULL, not a wrapped block");
    CHECK(kmalloc(4294967288u) == NULL, "malloc near 2^32 returns NULL");
    CHECK(kmalloc(4 * 1024 * 1024) == NULL, "malloc whole heap size returns NULL");
    CHECK(kmalloc(4 * 1024 * 1024 - 16 + 1) == NULL, "malloc one byte over max usable returns NULL");
    void* whole = kmalloc(4 * 1024 * 1024 - 16);
    CHECK(whole != NULL, "malloc max usable size succeeds");
    CHECK(kfree(whole) == 0, "free of max block succeeds");
    CHECK(heap_get_used() == 0, "heap empty after max block free");

    // Invalid and double frees must not corrupt the heap.
    void* q = kmalloc(32);
    CHECK(kfree(NULL) == -1, "free NULL is rejected");
    CHECK(kfree((void*)123) == -1, "free of a bogus address is rejected");
    CHECK(kfree((char*)q + 1) == -1, "free of a mid-block pointer is rejected");
    CHECK(kfree(q) == 0, "free of a live block still succeeds after bad frees");
    CHECK(kfree(q) == -2, "double free is reported");
    void* r = kmalloc(32);
    CHECK(r != NULL, "heap still works after bad and double frees");
    CHECK(kfree(r) == 0, "cleanup free succeeds");
    CHECK(heap_get_used() == 0, "heap empty after bad-free tests");

    // Coalescing across three blocks, freed out of order.
    void* a = kmalloc(64);
    void* b = kmalloc(64);
    void* c = kmalloc(64);
    CHECK(a && b && c, "three allocations succeed");
    CHECK(kfree(b) == 0 && kfree(a) == 0 && kfree(c) == 0, "three frees succeed");
    CHECK(heap_get_free_blocks() == 1, "out-of-order frees coalesce to one block");
    CHECK(heap_get_used() == 0, "heap empty after coalescing test");

    // Reuse after free.
    void* s1 = kmalloc(64);
    CHECK(kfree(s1) == 0, "free for reuse test succeeds");
    void* s2 = kmalloc(64);
    CHECK(s2 != NULL, "allocation after free reuses the heap");
    CHECK(kfree(s2) == 0, "reuse cleanup succeeds");

    // RAMFS: create, duplicate, long name, write, read, rewrite.
    CHECK(ramfs_create("hello.txt") == 0, "ramfs create succeeds");
    CHECK(ramfs_create("hello.txt") == -1, "ramfs duplicate create fails");
    CHECK(ramfs_create("12345678901234567890123456789012") == -2, "ramfs 32-char name is too long");
    CHECK(ramfs_write("hello.txt", "hello") == 0, "ramfs write succeeds");
    char out[256];
    CHECK(ramfs_read("hello.txt", out, sizeof(out)) == 5, "ramfs read returns 5 bytes");
    CHECK(strcmp(out, "hello") == 0, "ramfs read returns hello");
    CHECK(ramfs_write("hello.txt", "hello world") == 0, "ramfs rewrite succeeds");
    CHECK(ramfs_read("hello.txt", out, sizeof(out)) == 11, "ramfs rewrite read returns 11 bytes");
    CHECK(strcmp(out, "hello world") == 0, "ramfs rewrite returns new content");
    CHECK(ramfs_write("new.txt", "auto") == 0, "ramfs write to a missing file creates it");
    CHECK(ramfs_read("new.txt", out, sizeof(out)) == 4, "ramfs auto-created file reads back");
    CHECK(ramfs_create("empty.txt") == 0, "ramfs create empty file succeeds");
    CHECK(ramfs_read("empty.txt", out, sizeof(out)) == 0 && out[0] == 0, "ramfs read of empty file returns empty string");
    CHECK(ramfs_read("missing.txt", out, sizeof(out)) == -1, "ramfs read of missing file fails");
    CHECK(ramfs_read("hello.txt", out, 0) == -1, "ramfs read with max 0 fails instead of underflowing");
    CHECK(ramfs_delete("hello.txt") == 0, "ramfs delete succeeds");
    CHECK(ramfs_delete("hello.txt") == -1, "ramfs delete of missing file fails");
    ramfs_list(); // must not crash; output goes to stdout via stubs
    printf("PASS: ramfs_list runs without crashing\n");

    // RAMFS write under out-of-memory: the old data must survive.
    CHECK(ramfs_create("keep.txt") == 0, "ramfs create for OOM test succeeds");
    CHECK(ramfs_write("keep.txt", "keep me") == 0, "ramfs write for OOM test succeeds");
    // Exhaust the heap in stages: 64 KB blocks, then 1 KB blocks,
    // then 16-byte blocks, so the leftover fragment is under 32
    // bytes and even a small write cannot fit. Hogging with 64 KB
    // blocks alone leaves a ~64 KB fragment, which a small write
    // would happily use, and the OOM check below would not trigger.
    void* hogs[512];
    int hogged = 0;
    while (hogged < 512) {
        void* h = kmalloc(65536);
        if (!h) break;
        hogs[hogged++] = h;
    }
    while (hogged < 512) {
        void* h = kmalloc(1024);
        if (!h) break;
        hogs[hogged++] = h;
    }
    while (hogged < 512) {
        void* h = kmalloc(16);
        if (!h) break;
        hogs[hogged++] = h;
    }
    CHECK(hogged > 0, "heap exhaustion setup actually allocated blocks");
    CHECK(ramfs_write("keep.txt", "new content that cannot fit") == -2, "ramfs write under OOM fails");
    CHECK(ramfs_read("keep.txt", out, sizeof(out)) == 7, "ramfs read after failed write still returns old size");
    CHECK(strcmp(out, "keep me") == 0, "ramfs old content survives a failed write");
    for (int i = 0; i < hogged; i++) kfree(hogs[i]);
    CHECK(ramfs_write("keep.txt", "after free") == 0, "ramfs write succeeds again after freeing");
    CHECK(ramfs_read("keep.txt", out, sizeof(out)) == 10, "ramfs read after recovery returns new content");
    CHECK(ramfs_delete("keep.txt") == 0 && ramfs_delete("new.txt") == 0 && ramfs_delete("empty.txt") == 0, "ramfs cleanup deletes succeed");
    CHECK(heap_get_used() == 0, "heap empty at end of all tests");

    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}

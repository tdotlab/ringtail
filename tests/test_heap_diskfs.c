// RingTail host tests - heap and disk filesystem
//
// Runs the real kernel heap.c and diskfs.c on the host, in 32-bit
// mode, so the pointer-width assumptions in the kernel code hold
// exactly as they do on the target. Build and run with:
//   make run
// in this directory (needs gcc -m32, i.e. gcc-multilib on Ubuntu).
//
// Not covered here, on purpose: timer.c, keyboard.c, and ata.c
// use x86 port I/O, which faults in user mode, and paging.c
// touches CR0/CR3, which also faults, so they are QEMU-only.
// The kernel main loop and shell are also QEMU-only.
//
// The heap under test is a dedicated static array: heap_init
// starts the heap at _kernel_end, page-aligned, and the array is
// 8 KB larger than the heap, so alignment slack cannot spill
// into other globals.
//
// The disk under test is a mock: a 512-sector array in RAM, with
// the same RTFS layout diskfs.c expects (superblock sector 0,
// 64-entry directory in sectors 1-6, 8-sector bitmap in 7-14,
// data from sector 15). diskfs.c calls ata_read_sector and
// ata_write_sector, and this file provides those as stubs that
// copy to and from the mock array. That proves the filesystem
// logic (directory, bitmap, contiguous allocation, write-back,
// persistence across re-init) without real hardware. The ATA
// driver itself is QEMU-only, by the port I/O rule above.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Heap storage: heap.c declares extern char _kernel_end[] and
// builds a 4 MB heap starting at its page-aligned address.
char _kernel_end[4 * 1024 * 1024 + 8192];

// Mock disk: 512 sectors of 512 bytes.
#define MOCK_SECTORS 512
#define MOCK_DATA_START 15
static uint8_t mock_disk[MOCK_SECTORS * 512];

int ata_read_sector(uint32_t lba, uint8_t* buf) {
    if (lba >= MOCK_SECTORS) return -1;
    memcpy(buf, mock_disk + lba * 512, 512);
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t* buf) {
    if (lba >= MOCK_SECTORS) return -1;
    memcpy(mock_disk + lba * 512, buf, 512);
    return 0;
}

static void mock_disk_format(void) {
    memset(mock_disk, 0, sizeof(mock_disk));
    // Superblock fields, in diskfs.c's superblock_t order.
    memcpy(mock_disk + 0, "RTFS", 4);
    uint32_t* sb = (uint32_t*)(mock_disk + 4);
    sb[0] = 1;            // version
    sb[1] = MOCK_SECTORS; // total_sectors
    sb[2] = 1;            // dir_start
    sb[3] = 64;           // dir_count
    sb[4] = 7;            // bitmap_start
    sb[5] = 8;            // bitmap_sectors
    sb[6] = MOCK_DATA_START; // data_start
    sb[7] = 512;          // sector_size
}

// Stubs for the kernel print functions diskfs_list uses.
void print(const char* s) { fputs(s, stdout); }
void print_char(char c) { putchar(c); }

extern void heap_init(void);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern uint32_t heap_get_used(void);
extern uint32_t heap_get_free(void);
extern uint32_t heap_get_free_blocks(void);
extern void diskfs_init(void);
extern int diskfs_create(const char* name);
extern int diskfs_write(const char* name, const char* content);
extern int diskfs_read(const char* name, char* out, uint32_t max);
extern int diskfs_delete(const char* name);
extern void diskfs_list(void);
extern int diskfs_exists(const char* name);

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

    // DISKFS: format the mock disk, init, then the same
    // create/write/read/rewrite/delete shape ramfs tested.
    mock_disk_format();
    diskfs_init();
    CHECK(diskfs_exists("hello.txt") == 0, "diskfs file does not exist before create");
    CHECK(diskfs_create("hello.txt") == 0, "diskfs create succeeds");
    CHECK(diskfs_exists("hello.txt") == 1, "diskfs file exists after create");
    CHECK(diskfs_create("hello.txt") == -1, "diskfs duplicate create fails");
    CHECK(diskfs_create("12345678901234567890123456789012") == -2, "diskfs 32-char name is too long");
    CHECK(diskfs_write("hello.txt", "hello") == 0, "diskfs write succeeds");
    char out[2048];
    CHECK(diskfs_read("hello.txt", out, sizeof(out)) == 5, "diskfs read returns 5 bytes");
    CHECK(strcmp(out, "hello") == 0, "diskfs read returns hello");
    CHECK(diskfs_write("hello.txt", "hello world") == 0, "diskfs rewrite succeeds");
    CHECK(diskfs_read("hello.txt", out, sizeof(out)) == 11, "diskfs rewrite read returns 11 bytes");
    CHECK(strcmp(out, "hello world") == 0, "diskfs rewrite returns new content");
    CHECK(diskfs_write("new.txt", "auto") == 0, "diskfs write to a missing file creates it");
    CHECK(diskfs_read("new.txt", out, sizeof(out)) == 4, "diskfs auto-created file reads back");
    CHECK(diskfs_create("empty.txt") == 0, "diskfs create empty file succeeds");
    CHECK(diskfs_read("empty.txt", out, sizeof(out)) == 0 && out[0] == 0, "diskfs read of empty file returns empty string");
    CHECK(diskfs_read("missing.txt", out, sizeof(out)) == -1, "diskfs read of missing file fails");
    CHECK(diskfs_read("hello.txt", out, 0) == -1, "diskfs read with max 0 fails instead of underflowing");
    CHECK(diskfs_delete("hello.txt") == 0, "diskfs delete succeeds");
    CHECK(diskfs_delete("hello.txt") == -1, "diskfs delete of missing file fails");
    diskfs_list(); // must not crash; output goes to stdout via stubs
    printf("PASS: diskfs_list runs without crashing\n");

    // Multi-sector file: 1200 bytes spans 3 sectors, proving
    // contiguous allocation and sector-by-sector read/write.
    static char big[1201];
    for (int i = 0; i < 1200; i++) big[i] = 'A' + (i % 26);
    big[1200] = 0;
    CHECK(diskfs_write("big.txt", big) == 0, "diskfs multi-sector write succeeds");
    CHECK(diskfs_read("big.txt", out, sizeof(out)) == 1200, "diskfs multi-sector read returns 1200 bytes");
    CHECK(strcmp(out, big) == 0, "diskfs multi-sector content matches");
    CHECK(diskfs_delete("big.txt") == 0, "diskfs multi-sector delete succeeds");
    CHECK(diskfs_write("big2.txt", big) == 0, "diskfs write after delete reuses freed sectors");
    CHECK(diskfs_read("big2.txt", out, sizeof(out)) == 1200, "diskfs reused-sector file reads back");

    // Persistence across re-init: the mock disk still holds the
    // files, so a fresh diskfs_init must find them again. This
    // is the host-side version of the QEMU reboot test.
    diskfs_init();
    CHECK(diskfs_exists("new.txt") == 1, "diskfs file survives re-init");
    CHECK(diskfs_read("new.txt", out, sizeof(out)) == 4, "diskfs re-init read returns old content");
    CHECK(strcmp(out, "auto") == 0, "diskfs re-init content matches");
    CHECK(diskfs_exists("big2.txt") == 1, "diskfs multi-sector file survives re-init");

    // Disk-full: the mock disk has 497 data sectors. A file
    // needing more than the remaining free run must fail, and
    // the old files must survive the failed write.
    static char huge[200 * 1024 + 1];
    for (int i = 0; i < 200 * 1024; i++) huge[i] = 'x';
    huge[200 * 1024] = 0;
    CHECK(diskfs_write("huge.txt", huge) == 0, "diskfs large file write succeeds");
    CHECK(diskfs_write("toobig.txt", huge) == -2, "diskfs write with no contiguous run fails");
    CHECK(diskfs_read("huge.txt", out, 16) == 15, "diskfs old file still readable after failed write");
    CHECK(diskfs_delete("huge.txt") == 0 && diskfs_delete("new.txt") == 0 &&
          diskfs_delete("empty.txt") == 0 && diskfs_delete("big2.txt") == 0,
          "diskfs cleanup deletes succeed");

    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}

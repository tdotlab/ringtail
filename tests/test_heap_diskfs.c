// RingTail host tests - heap and disk filesystem (FAT16)
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
// The disk under test is a mock: an 8192-sector array in RAM,
// with the FAT16 layout diskfs.c expects (boot sector with a
// BPB, two 32-sector FAT copies, a 512-entry root directory,
// data from sector 97, 8095 clusters of 1 sector each). The
// size is deliberate: a smaller mock disk would have fewer
// than 4085 clusters and would be FAT12, which diskfs.c
// correctly rejects, so the mock must be big enough to be
// FAT16. diskfs.c calls ata_read_sector and ata_write_sector,
// and this file provides those as stubs that copy to and from
// the mock array. That proves the filesystem logic (BPB
// parsing, 8.3 names, cluster chains, fragmentation, write-back,
// persistence across re-init) without real hardware. The ATA
// driver itself is QEMU-only, by the port I/O rule above.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Heap storage: heap.c declares extern char _kernel_end[] and
// builds a 4 MB heap starting at its page-aligned address.
char _kernel_end[4 * 1024 * 1024 + 8192];

// Mock disk: 8192 sectors of 512 bytes (4 MB).
#define MOCK_SECTORS 8192
#define MOCK_FAT_SECTORS 32
#define MOCK_FAT_START 1
#define MOCK_ROOT_START 65
#define MOCK_DATA_START 97
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

static void mock_put16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
}

static void mock_disk_format(void) {
    memset(mock_disk, 0, sizeof(mock_disk));
    uint8_t* b = mock_disk;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "RINGTAIL", 8);
    mock_put16(b, 11, 512);
    b[13] = 1;
    mock_put16(b, 14, 1);
    b[16] = 2;
    mock_put16(b, 17, 512);
    mock_put16(b, 19, MOCK_SECTORS);
    b[21] = 0xF8;
    mock_put16(b, 22, MOCK_FAT_SECTORS);
    mock_put16(b, 24, 63);
    mock_put16(b, 26, 255);
    b[36] = 0x80;
    b[38] = 0x29;
    memcpy(b + 43, "RINGTAIL   ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    // FAT copies: entry 0 media, entry 1 end-of-chain, rest free.
    for (int f = 0; f < 2; f++) {
        uint8_t* fat = mock_disk + (MOCK_FAT_START + f * MOCK_FAT_SECTORS) * 512;
        mock_put16(fat, 0, 0xFFF8);
        mock_put16(fat, 2, 0xFFFF);
    }
    // Volume label entry first, matching the BPB label, as
    // mkdisk.py does. The kernel must skip it everywhere.
    uint8_t* vol = mock_disk + MOCK_ROOT_START * 512;
    memcpy(vol, "RINGTAIL   ", 11);
    vol[11] = 0x08;
}

// A FAT12-shaped mock (512 sectors, 461 data clusters) for
// the rejection test at the end: diskfs.c must refuse it
// instead of misreading it as FAT16.
static void mock_disk_format_fat12(void) {
    memset(mock_disk, 0, sizeof(mock_disk));
    uint8_t* b = mock_disk;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "RINGTAIL", 8);
    mock_put16(b, 11, 512);
    b[13] = 1;
    mock_put16(b, 14, 1);
    b[16] = 2;
    mock_put16(b, 17, 512);
    mock_put16(b, 19, 512);
    b[21] = 0xF8;
    mock_put16(b, 22, 9);
    b[510] = 0x55; b[511] = 0xAA;
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
extern int diskfs_size(const char* name, uint32_t* size_out);
extern int diskfs_list_at(int index, char* name_out);

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

// Follow a file's FAT chain in the mock disk, for the
// fragmentation test. name is a user name (e.g. "frag.txt");
// it is converted to raw 8.3 here, the same way diskfs.c
// does. Returns chain length, or -1 if not found.
static int mock_chain(const char* name, uint32_t* chain, int max) {
    char base[9], ext[4];
    const char* dot = strchr(name, '.');
    int bl = dot ? (int)(dot - name) : (int)strlen(name);
    int el = dot ? (int)strlen(dot + 1) : 0;
    if (bl < 1 || bl > 8 || el > 3) return -1;
    memset(base, ' ', 8); base[8] = 0;
    memset(ext, ' ', 3); ext[3] = 0;
    for (int i = 0; i < bl; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        base[i] = c;
    }
    for (int i = 0; i < el; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        ext[i] = c;
    }
    uint8_t raw[11];
    memcpy(raw, base, 8);
    memcpy(raw + 8, ext, 3);
    uint8_t* root = mock_disk + MOCK_ROOT_START * 512;
    uint8_t* fat = mock_disk + MOCK_FAT_START * 512;
    for (int i = 0; i < 512; i++) {
        uint8_t* e = root + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[11] == 0x0F) continue;
        if (memcmp(e, raw, 11) != 0) continue;
        uint32_t cur = e[26] | (e[27] << 8);
        int n = 0;
        while (cur >= 2 && n < max) {
            chain[n++] = cur;
            uint16_t nxt = fat[cur * 2] | (fat[cur * 2 + 1] << 8);
            if (nxt >= 0xFFF8) break;
            cur = nxt;
        }
        return n;
    }
    return -1;
}

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

    // DISKFS (FAT16): format the mock disk, init, then the same
    // create/write/read/rewrite/delete shape the RTFS tests used.
    mock_disk_format();
    diskfs_init();
    CHECK(diskfs_exists("hello.txt") == 0, "diskfs file does not exist before create");
    CHECK(diskfs_create("hello.txt") == 0, "diskfs create succeeds");
    CHECK(diskfs_exists("hello.txt") == 1, "diskfs file exists after create");
    CHECK(diskfs_create("hello.txt") == -1, "diskfs duplicate create fails");
    CHECK(diskfs_create("12345678901234567890123456789012") == -2, "diskfs 32-char name is too long for 8.3");
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

    // 8.3 naming: too-long base, multiple dots, forbidden
    // characters, and case-insensitive lookup.
    CHECK(diskfs_create("a.txt") == 0, "diskfs 8.3 short name creates");
    CHECK(diskfs_create("TOOLONGNAME.txt") == -2, "diskfs 12-char base is too long for 8.3");
    CHECK(diskfs_create("a.b.c") == -2, "diskfs multiple dots rejected");
    CHECK(diskfs_create("bad/name") == -2, "diskfs slash in name rejected");
    CHECK(diskfs_create("lower.txt") == 0, "diskfs lowercase name creates");
    CHECK(diskfs_write("lower.txt", "low") == 0, "diskfs lowercase write succeeds");
    CHECK(diskfs_exists("LOWER.TXT") == 1, "diskfs uppercase lookup finds lowercase file");
    CHECK(diskfs_read("LOWER.TXT", out, sizeof(out)) == 3 && strcmp(out, "low") == 0, "diskfs uppercase read returns lowercase file content");
    CHECK(diskfs_delete("a.txt") == 0 && diskfs_delete("lower.txt") == 0, "diskfs 8.3 cleanup deletes succeed");

    // Multi-cluster file: 1200 bytes spans 3 clusters, proving
    // chain allocation and sector-by-sector read/write.
    static char big[1201];
    for (int i = 0; i < 1200; i++) big[i] = 'A' + (i % 26);
    big[1200] = 0;
    CHECK(diskfs_write("big.txt", big) == 0, "diskfs multi-cluster write succeeds");
    CHECK(diskfs_read("big.txt", out, sizeof(out)) == 1200, "diskfs multi-cluster read returns 1200 bytes");
    CHECK(strcmp(out, big) == 0, "diskfs multi-cluster content matches");
    CHECK(diskfs_delete("big.txt") == 0, "diskfs multi-cluster delete succeeds");
    CHECK(diskfs_write("big2.txt", big) == 0, "diskfs write after delete reuses freed clusters");
    CHECK(diskfs_read("big2.txt", out, sizeof(out)) == 1200, "diskfs reused-cluster file reads back");

    // Persistence across re-init: the mock disk still holds the
    // files, so a fresh diskfs_init must find them again. This
    // is the host-side version of the QEMU reboot test.
    diskfs_init();
    CHECK(diskfs_exists("new.txt") == 1, "diskfs file survives re-init");
    CHECK(diskfs_read("new.txt", out, sizeof(out)) == 4, "diskfs re-init read returns old content");
    CHECK(strcmp(out, "auto") == 0, "diskfs re-init content matches");
    CHECK(diskfs_exists("big2.txt") == 1, "diskfs multi-cluster file survives re-init");

    // File size, for seek from the end: directory entry only,
    // no whole-file read, so it works for empty and small
    // files alike, and (below) for larger-than-read_all files.
    uint32_t sz = 0;
    CHECK(diskfs_size("new.txt", &sz) == 0 && sz == 4, "diskfs size of new.txt is 4");
    CHECK(diskfs_size("big2.txt", &sz) == 0 && sz == 1200, "diskfs size of big2.txt is 1200");
    CHECK(diskfs_size("empty.txt", &sz) == 0 && sz == 0, "diskfs size of empty file is 0");
    CHECK(diskfs_size("missing.txt", &sz) == -1, "diskfs size of missing file fails");

    // Size beyond the read_all cap: a 100,000-byte file is
    // larger than the 64 KB whole-file read cap, so a size
    // lookup must still succeed even though a whole-file
    // read would fail. This is why seek from the end uses
    // size, not read_all.
    static char huge[100001];
    for (int i = 0; i < 100000; i++) huge[i] = 'x';
    huge[100000] = 0;
    CHECK(diskfs_write("huge.txt", huge) == 0, "diskfs 100KB write succeeds");
    CHECK(diskfs_size("huge.txt", &sz) == 0 && sz == 100000, "diskfs size of 100KB file is 100000");
    CHECK(diskfs_delete("huge.txt") == 0, "diskfs 100KB cleanup delete succeeds");

    // Fragmentation, on a fresh disk so cluster numbers are
    // deterministic: A=2,3,4; B=5,6,7; C=8,9,10. Delete B,
    // then write D (2500 bytes, 5 clusters). First-fit reuse
    // must give D the chain 5,6,7,11,12, jumping over C's
    // clusters: a chain, not a contiguous run, which RTFS
    // could not have produced.
    mock_disk_format();
    diskfs_init();
    static char frag[2501];
    for (int i = 0; i < 2500; i++) frag[i] = 'a' + (i % 26);
    frag[2500] = 0;
    CHECK(diskfs_write("fa.txt", big) == 0, "diskfs frag A write succeeds");
    CHECK(diskfs_write("fb.txt", big) == 0, "diskfs frag B write succeeds");
    CHECK(diskfs_write("fc.txt", big) == 0, "diskfs frag C write succeeds");
    CHECK(diskfs_delete("fb.txt") == 0, "diskfs frag B delete succeeds");
    CHECK(diskfs_write("frag.txt", frag) == 0, "diskfs fragmented write succeeds");
    // Verify the full 2500-byte content with a larger buffer
    // (the 2048-byte out buffer above would truncate it).
    static char fragout[2600];
    CHECK(diskfs_read("frag.txt", fragout, sizeof(fragout)) == 2500, "diskfs fragmented read returns 2500 bytes");
    CHECK(strcmp(fragout, frag) == 0, "diskfs fragmented content matches");
    uint32_t chain[8];
    int nchain = mock_chain("frag.txt", chain, 8);
    CHECK(nchain == 5 && chain[0] == 5 && chain[1] == 6 && chain[2] == 7 &&
          chain[3] == 11 && chain[4] == 12,
          "diskfs fragmented chain is 5,6,7,11,12, not contiguous");

    // Disk-full: after the frag files (11 clusters used),
    // 8084 clusters remain (4,139,008 bytes). A file one byte
    // larger needs 8085 clusters and must fail, leaving the
    // old files intact.
    static char toobig[4139009 + 1];
    for (int i = 0; i < 4139009; i++) toobig[i] = 'x';
    toobig[4139009] = 0;
    CHECK(diskfs_write("toobig.txt", toobig) == -2, "diskfs write larger than free space fails");
    CHECK(diskfs_read("frag.txt", fragout, sizeof(fragout)) == 2500, "diskfs old file still readable after failed write");
    CHECK(diskfs_delete("fa.txt") == 0 && diskfs_delete("fc.txt") == 0 &&
          diskfs_delete("frag.txt") == 0,
          "diskfs frag cleanup deletes succeed");
    CHECK(diskfs_delete("new.txt") == -1, "diskfs frag disk has no leftover new.txt");

    // LIST by index, on a fresh disk, so slot order is
    // creation order and the expectations are exact.
    // The volume entry at slot 0 must be skipped, so the
    // first regular file is index 0, not the volume.
    mock_disk_format();
    diskfs_init();
    CHECK(diskfs_list_at(0, out) == -1, "diskfs list_at on empty disk fails");
    CHECK(diskfs_create("a.txt") == 0 && diskfs_create("b.txt") == 0 &&
          diskfs_create("c.txt") == 0 && diskfs_create("prog") == 0,
          "diskfs list setup creates succeed");
    CHECK(diskfs_list_at(0, out) == 0 && strcmp(out, "A.TXT") == 0, "diskfs list_at 0 is A.TXT");
    CHECK(diskfs_list_at(1, out) == 0 && strcmp(out, "B.TXT") == 0, "diskfs list_at 1 is B.TXT");
    CHECK(diskfs_list_at(2, out) == 0 && strcmp(out, "C.TXT") == 0, "diskfs list_at 2 is C.TXT");
    CHECK(diskfs_list_at(3, out) == 0 && strcmp(out, "PROG") == 0, "diskfs list_at 3 is PROG, no dot");
    CHECK(diskfs_list_at(4, out) == -1, "diskfs list_at past last file fails");
    CHECK(diskfs_list_at(-1, out) == -1, "diskfs list_at negative index fails");
    CHECK(diskfs_delete("b.txt") == 0, "diskfs list setup delete succeeds");
    CHECK(diskfs_list_at(0, out) == 0 && strcmp(out, "A.TXT") == 0, "diskfs list_at 0 still A.TXT after delete");
    CHECK(diskfs_list_at(1, out) == 0 && strcmp(out, "C.TXT") == 0, "diskfs list_at 1 is C.TXT after delete, skipping deleted");
    CHECK(diskfs_list_at(2, out) == 0 && strcmp(out, "PROG") == 0, "diskfs list_at 2 is PROG after delete");
    CHECK(diskfs_list_at(3, out) == -1, "diskfs list_at past last file fails after delete");

    // FAT12 rejection: a small-cluster disk must be refused,
    // not misread as FAT16.
    mock_disk_format_fat12();
    diskfs_init();
    CHECK(diskfs_exists("hello.txt") == 0, "diskfs FAT12 disk has no files");
    CHECK(diskfs_create("hello.txt") == -4, "diskfs FAT12 create fails with no disk");
    CHECK(diskfs_size("hello.txt", &sz) == -4, "diskfs FAT12 size fails with no disk");
    CHECK(diskfs_list_at(0, out) == -1, "diskfs FAT12 list_at fails with no disk");
    diskfs_list(); // must print "no disk", not crash
    printf("PASS: diskfs_list on FAT12 disk runs without crashing\n");

    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}

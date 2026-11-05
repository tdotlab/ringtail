// Host tests for the kernel heap (heap.c) and the FAT16 filesystem
// (diskfs.c), built 32-bit so pointer sizes match the target.
//
// diskfs.c talks to the disk through ata_read/ata_write/ata_flush;
// here those copy to and from a RAM image, with a switch that makes
// writes fail after a set count so error paths can be exercised.
// After the run the image is saved to test_disk.img, and the
// Makefile hands it to fsck.fat to prove the result is a valid FAT
// volume by another implementation's standards.
//
// Hardware drivers, paging and the scheduler need a real (emulated)
// machine and are covered by qemu_test.py instead.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mm.h"
#include "fs.h"
#include "ringtail/abi.h"

static int passed, failed;
#define CHECK(cond, what) do { \
    if (cond) passed++; \
    else { failed++; printf("  FAIL  %s (line %d)\n", what, __LINE__); } \
} while (0)

// ---- Mock disk ----

#define MAX_SECTORS 32768u  // 16 MB
static uint8_t disk[MAX_SECTORS * 512];
static uint32_t disk_sectors;
static int writes_left = -1;  // -1: never fail
static uint32_t flushes;

int ata_present(void) { return disk_sectors != 0; }
uint32_t ata_sectors(void) { return disk_sectors; }
int ata_flush(void) { flushes++; return 0; }

int ata_read(uint32_t lba, uint32_t count, void* buf) {
    if (lba + count > disk_sectors) return -EIO;
    memcpy(buf, disk + lba * 512, count * 512);
    return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void* buf) {
    if (lba + count > disk_sectors) return -EIO;
    if (writes_left == 0) return -EIO;
    if (writes_left > 0) writes_left--;
    memcpy(disk + lba * 512, buf, count * 512);
    return 0;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

// Format a FAT16 volume the way mkfs.fat would: one reserved sector,
// two FATs, a 512-entry root, the FAT sized to cover every cluster.
static void format(uint32_t sectors, uint32_t spc) {
    disk_sectors = sectors;
    memset(disk, 0, sectors * 512);
    uint32_t rsec = 32, fsz = 1;
    for (;;) {
        uint32_t clusters = (sectors - 1 - 2 * fsz - rsec) / spc;
        if (fsz * 256 >= clusters + 2) break;
        fsz++;
    }
    uint8_t* b = disk;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "RINGTAIL", 8);
    put16(b + 11, 512);
    b[13] = (uint8_t)spc;
    put16(b + 14, 1);
    b[16] = 2;
    put16(b + 17, 512);
    if (sectors < 65536) put16(b + 19, (uint16_t)sectors); else put32(b + 32, sectors);
    b[21] = 0xF8;
    put16(b + 22, (uint16_t)fsz);
    put16(b + 24, 63);
    put16(b + 26, 16);
    b[36] = 0x80;
    b[38] = 0x29;
    put32(b + 39, 0x12345678);
    memcpy(b + 43, "RINGTAIL   ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    for (uint32_t f = 0; f < 2; f++) {
        uint8_t* fat = disk + (1 + f * fsz) * 512;
        put16(fat, 0xFFF8);
        put16(fat + 2, 0xFFFF);
    }
    // A volume label entry, which every directory walk must skip.
    uint8_t* vol = disk + (1 + 2 * fsz) * 512;
    memcpy(vol, "RINGTAIL   ", 11);
    vol[11] = 0x08;
}

// ---- Heap ----

static uint8_t heap_area[1024 * 1024 + 64];

static void test_heap(void) {
    heap_init(heap_area + 3, sizeof heap_area - 3);  // deliberately misaligned
    heap_stats_t s0, s;
    heap_get_stats(&s0);
    CHECK(s0.start % 16 == 0, "heap start is aligned");
    CHECK(s0.used == 0 && s0.free_blocks == 1, "fresh heap is one free block");

    void* a = kmalloc(100);
    void* b = kmalloc(200);
    void* c = kmalloc(300);
    CHECK(a && b && c, "three allocations succeed");
    CHECK(((uintptr_t)a | (uintptr_t)b | (uintptr_t)c) % 16 == 0, "allocations are 16-byte aligned");
    CHECK((uint8_t*)a + 100 <= (uint8_t*)b && (uint8_t*)b + 200 <= (uint8_t*)c, "allocations do not overlap");
    memset(a, 0xAA, 100); memset(b, 0xBB, 200); memset(c, 0xCC, 300);
    CHECK(((uint8_t*)a)[99] == 0xAA && ((uint8_t*)b)[0] == 0xBB, "neighbours keep their contents");

    CHECK(kmalloc(0) == 0, "zero-byte allocation fails");
    CHECK(kmalloc(sizeof heap_area) == 0, "allocation bigger than the heap fails");
    CHECK(kfree(0) == -1, "free of NULL is refused");
    CHECK(kfree((uint8_t*)b + 16) == -1, "free of an interior pointer is refused");
    CHECK(kfree(heap_area) == -1, "free of a non-heap pointer is refused");

    CHECK(kfree(b) == 0, "free succeeds");
    CHECK(kfree(b) == -2, "double free is detected");
    void* b2 = kmalloc(150);
    CHECK(b2 == b, "first fit reuses the freed hole");
    kfree(b2);
    kfree(a);
    kfree(c);
    heap_get_stats(&s);
    CHECK(s.used == 0 && s.free_blocks == 1 && s.largest_free == s0.largest_free,
          "freeing everything coalesces back to one block");

    uint8_t* z = kmalloc(64);
    memset(z, 0x5A, 64);
    kfree(z);
    uint8_t* z2 = kzalloc(64);
    int zero = 1;
    for (int i = 0; i < 64; i++) zero &= z2[i] == 0;
    CHECK(z2 == z && zero, "kzalloc returns zeroed memory");
    kfree(z2);

    // Fill the heap with small blocks, free every other one, then
    // the rest, and check it all merges again.
    void* p[4096];
    int n = 0;
    while (n < 4096 && (p[n] = kmalloc(200))) n++;
    CHECK(n > 1000, "heap holds many small blocks");
    for (int i = 0; i < n; i += 2) kfree(p[i]);
    heap_get_stats(&s);
    CHECK(s.free_blocks >= (uint32_t)n / 2, "freed holes stay separate while their neighbours live");
    for (int i = 1; i < n; i += 2) kfree(p[i]);
    heap_get_stats(&s);
    CHECK(s.used == 0 && s.free_blocks == 1, "and merge once the neighbours go");
}

// ---- Filesystem ----

static int read_all(const char* name, char* buf, uint32_t max) {
    int node = diskfs_lookup(DISKFS_ROOT, name);
    if (node < 0) return node;
    int r = diskfs_pread(node, buf, max, 0);
    diskfs_unref(node);
    return r;
}

// Create a file (or fail) without keeping it open.
static int touch(const char* path) {
    int node = diskfs_create(DISKFS_ROOT, path);
    if (node < 0) return node;
    diskfs_unref(node);
    return 0;
}

static void test_names(void) {
    CHECK(diskfs_name_ok("HELLO.TXT"), "plain 8.3 name is fine");
    CHECK(diskfs_name_ok("readme"), "lowercase without an extension is fine");
    CHECK(!diskfs_name_ok(""), "empty name is rejected");
    CHECK(!diskfs_name_ok("TOOLONGNAME.TXT"), "nine-character base is rejected");
    CHECK(!diskfs_name_ok("A.TEXT"), "four-character extension is rejected");
    CHECK(!diskfs_name_ok("A.B.C"), "two dots are rejected");
    CHECK(!diskfs_name_ok("NAME."), "trailing dot is rejected");
    CHECK(!diskfs_name_ok(".TXT"), "empty base is rejected");
    CHECK(!diskfs_name_ok("A B"), "space is rejected");
    CHECK(!diskfs_name_ok("A*B") && !diskfs_name_ok("A/B") && !diskfs_name_ok("A:B"),
          "reserved characters are rejected");
    CHECK(!diskfs_name_ok("A\x01") && !diskfs_name_ok("A\x7F") && !diskfs_name_ok("A\xE5"),
          "control and high bytes are rejected");
}

static void test_basic(void) {
    char buf[8192];
    format(16384, 1);
    CHECK(diskfs_init() == 0, "fresh FAT16 volume mounts");
    uint32_t clusters, free0, cb;
    diskfs_stats(&clusters, &free0, &cb);
    CHECK(clusters >= 4085 && free0 == clusters && cb == 512, "stats show an empty disk");

    int cursor = 0;
    dirent_t d;
    CHECK(diskfs_readdir(DISKFS_ROOT, &cursor, &d) == -ENOENT, "the volume label is not listed as a file");
    CHECK(diskfs_lookup(DISKFS_ROOT, "HELLO.TXT") == -ENOENT, "missing file is not found");
    CHECK(diskfs_lookup(DISKFS_ROOT, "BAD NAME") == -EINVAL, "lookup of an invalid name is -EINVAL");

    int node = diskfs_create(DISKFS_ROOT, "hello.txt");
    CHECK(node >= 0, "create succeeds");
    CHECK(diskfs_create(DISKFS_ROOT, "HELLO.TXT") == -EEXIST, "create is case-insensitive and refuses duplicates");
    CHECK(diskfs_lookup(DISKFS_ROOT, "Hello.Txt") == node, "lookup is case-insensitive");
    CHECK(diskfs_refs(node) == 2, "and a lookup of an open file shares its node");
    diskfs_unref(node);
    CHECK(diskfs_size(node) == 0, "new file is empty");
    CHECK(diskfs_pread(node, buf, sizeof buf, 0) == 0, "reading an empty file gives 0 bytes");

    CHECK(diskfs_pwrite(node, "hello", 5, 0) == 5, "write 5 bytes");
    CHECK(diskfs_size(node) == 5, "size is 5");
    memset(buf, 0, sizeof buf);
    CHECK(diskfs_pread(node, buf, sizeof buf, 0) == 5 && memcmp(buf, "hello", 5) == 0, "read them back");
    CHECK(diskfs_pread(node, buf, 3, 2) == 3 && memcmp(buf, "llo", 3) == 0, "read at an offset");
    CHECK(diskfs_pread(node, buf, 10, 5) == 0 && diskfs_pread(node, buf, 10, 99) == 0, "read at or past the end gives 0");

    // A write that starts past the end must zero the gap, across a
    // cluster boundary.
    CHECK(diskfs_pwrite(node, "X", 1, 1500) == 1, "write past the end");
    CHECK(diskfs_size(node) == 1501, "size grows to cover it");
    CHECK(diskfs_pread(node, buf, sizeof buf, 0) == 1501, "whole file reads");
    int gap_zero = 1;
    for (int i = 5; i < 1500; i++) gap_zero &= buf[i] == 0;
    CHECK(gap_zero && buf[1500] == 'X', "the gap reads as zeros");

    // Multi-cluster data with a pattern, overwritten in the middle.
    static char big[6000];
    for (int i = 0; i < 6000; i++) big[i] = (char)('a' + i % 26);
    CHECK(diskfs_pwrite(node, big, 6000, 0) == 6000, "6000-byte write over 12 clusters");
    CHECK(diskfs_pwrite(node, "MIDDLE", 6, 2047) == 6, "overwrite straddling a cluster boundary");
    memcpy(big + 2047, "MIDDLE", 6);
    memset(buf, 0, sizeof buf);
    CHECK(diskfs_pread(node, buf, sizeof buf, 0) == 6000 && memcmp(buf, big, 6000) == 0, "pattern survives intact");

    CHECK(diskfs_truncate(node, 700) == 0 && diskfs_size(node) == 700, "truncate shrinks");
    uint32_t f;
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0 - 2, "truncate releases the clusters past the end");
    CHECK(diskfs_truncate(node, 0) == 0 && diskfs_size(node) == 0, "truncate to zero");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "an empty file owns no clusters");

    CHECK(diskfs_write_file(DISKFS_ROOT, "notes.txt", "first", 5) == 0, "write_file creates a file");
    CHECK(diskfs_write_file(DISKFS_ROOT, "notes.txt", "second version", 14) == 0, "write_file replaces it");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("NOTES.TXT", buf, sizeof buf) == 14 && strcmp(buf, "second version") == 0, "the new contents are there");
    CHECK(diskfs_write_file(DISKFS_ROOT, "notes.txt", "", 0) == 0 && read_all("NOTES.TXT", buf, sizeof buf) == 0,
          "write_file with no data empties the file");

    // Open files cannot be deleted.
    int n2 = diskfs_lookup(DISKFS_ROOT, "NOTES.TXT");
    CHECK(diskfs_refs(n2) == 1, "a lookup holds a reference");
    CHECK(diskfs_unlink(DISKFS_ROOT, "NOTES.TXT") == -EBUSY, "unlink of an open file is -EBUSY");
    diskfs_unref(n2);
    CHECK(diskfs_unlink(DISKFS_ROOT, "NOTES.TXT") == 0, "unlink once closed");
    CHECK(diskfs_lookup(DISKFS_ROOT, "NOTES.TXT") == -ENOENT, "it is gone");
    CHECK(diskfs_unlink(DISKFS_ROOT, "NOTES.TXT") == -ENOENT, "second unlink is -ENOENT");

    // Directory listing.
    diskfs_write_file(DISKFS_ROOT, "A.TXT", "a", 1);
    diskfs_write_file(DISKFS_ROOT, "B.TXT", "bb", 2);
    int count = 0, saw_b = 0;
    cursor = 0;
    while (diskfs_readdir(DISKFS_ROOT, &cursor, &d) == 0) {
        count++;
        if (strcmp(d.name, "B.TXT") == 0 && d.size == 2 && d.flags == 0) saw_b = 1;
    }
    CHECK(count == 3 && saw_b, "readdir lists every file with its size");

    // Everything must survive a remount from the disk image.
    diskfs_pwrite(node, big, 6000, 0);
    CHECK(diskfs_init() == 0, "remount");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("HELLO.TXT", buf, sizeof buf) == 6000 && memcmp(buf, big, 6000) == 0, "data survives a remount");
    CHECK(read_all("B.TXT", buf, sizeof buf) == 2 && memcmp(buf, "bb", 2) == 0, "small file survives too");
    CHECK(memcmp(disk + 512, disk + 512 + 512 * ((uint32_t)disk[22] | disk[23] << 8),
                 512 * ((uint32_t)disk[22] | disk[23] << 8)) == 0, "both FAT copies match");
}

static void test_full_disk(void) {
    format(16384, 1);
    diskfs_init();
    uint32_t clusters, free0, f, cb;
    diskfs_stats(&clusters, &free0, &cb);
    diskfs_write_file(DISKFS_ROOT, "KEEP.TXT", "keep me", 7);
    diskfs_stats(&clusters, &free0, &cb);

    static uint8_t huge[9 * 1024 * 1024];
    memset(huge, 'Z', sizeof huge);
    CHECK(diskfs_write_file(DISKFS_ROOT, "HUGE.BIN", huge, sizeof huge) == -ENOSPC, "write bigger than the disk is -ENOSPC");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "a failed write_file gives back every cluster it took");
    CHECK(diskfs_lookup(DISKFS_ROOT, "HUGE.BIN") == -ENOENT, "and leaves no half-made file");

    int node = diskfs_create(DISKFS_ROOT, "FILL.BIN");
    int r = diskfs_pwrite(node, huge, sizeof huge, 0);
    CHECK(r > 0 && (uint32_t)r < sizeof huge, "pwrite to a full disk writes what fits");
    CHECK(diskfs_size(node) == (uint32_t)r, "and the size says how much");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == 0, "the disk is now full");
    CHECK(diskfs_pwrite(node, "more", 4, (uint32_t)r) == -ENOSPC, "further growth is -ENOSPC");
    CHECK(touch("EMPTY.TXT") == 0, "empty files still fit on a full disk");
    CHECK(diskfs_mkdir(DISKFS_ROOT, "NOROOM") == -ENOSPC, "a directory needs a cluster, so it does not");
    CHECK(diskfs_lookup(DISKFS_ROOT, "NOROOM") == -ENOENT, "and leaves no entry behind");
    diskfs_unref(node);
    CHECK(diskfs_unlink(DISKFS_ROOT, "FILL.BIN") == 0, "deleting the filler");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "frees all of it");
    char buf[16] = {0};
    CHECK(read_all("KEEP.TXT", buf, sizeof buf) == 7 && strcmp(buf, "keep me") == 0, "the other file is untouched");
}

static void test_io_errors(void) {
    char buf[64];
    format(16384, 1);
    diskfs_init();
    diskfs_write_file(DISKFS_ROOT, "SAFE.TXT", "original", 8);

    // Fail the very first write: the new data never lands, so the
    // directory must still point at the old data.
    writes_left = 0;
    CHECK(diskfs_write_file(DISKFS_ROOT, "SAFE.TXT", "replacement", 11) == -EIO, "disk error is reported");
    writes_left = -1;
    CHECK(diskfs_init() == 0, "remount after the error");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("SAFE.TXT", buf, sizeof buf) == 8 && strcmp(buf, "original") == 0,
          "a failed replace leaves the old contents");

    // Root directory capacity: 512 entries, one is the label.
    int made = 0;
    for (int i = 0; i < 600; i++) {
        char name[16];
        snprintf(name, sizeof name, "F%d.TXT", i);
        if (touch(name) == 0) made++;
    }
    CHECK(made == 512 - 2, "the root directory fills at its entry count");
    CHECK(diskfs_create(DISKFS_ROOT, "ONEMORE.TXT") == -ENOSPC, "and then create is -ENOSPC");
    CHECK(diskfs_mkdir(DISKFS_ROOT, "ONEMORE") == -ENOSPC, "and so is mkdir");
}

static int count_entries(int dir) {
    dirent_t d;
    int cursor = 0, n = 0;
    while (diskfs_readdir(dir, &cursor, &d) == 0) n++;
    return n;
}

static void test_dirs(void) {
    char buf[256];
    format(16384, 1);  // 512-byte clusters hold 16 entries, so directories grow often
    diskfs_init();
    uint32_t clusters, free0, f, cb;
    diskfs_stats(&clusters, &free0, &cb);

    CHECK(diskfs_mkdir(DISKFS_ROOT, "docs") == 0, "mkdir makes a directory");
    CHECK(diskfs_mkdir(DISKFS_ROOT, "DOCS") == -EEXIST, "a second mkdir of the name is -EEXIST");
    CHECK(touch("DOCS") == -EEXIST, "a file cannot take a directory's name");
    CHECK(diskfs_lookup(DISKFS_ROOT, "DOCS") == -EISDIR, "a directory does not open as a file");
    CHECK(diskfs_unlink(DISKFS_ROOT, "DOCS") == -EISDIR, "unlink refuses a directory");
    CHECK(diskfs_mkdir(DISKFS_ROOT, "/") == -EEXIST && diskfs_mkdir(DISKFS_ROOT, "..") == -EEXIST,
          "mkdir of / or .. is -EEXIST");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0 - 1, "an empty directory takes one cluster");
    dirent_t d;
    int cursor = 0;
    CHECK(diskfs_readdir(DISKFS_ROOT, &cursor, &d) == 0 && strcmp(d.name, "DOCS") == 0 &&
          d.flags == DIRENT_DIR && d.size == 0, "readdir marks it as a directory");

    CHECK(diskfs_write_file(DISKFS_ROOT, "docs/a.txt", "inside", 6) == 0, "write a file inside it");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("DOCS/A.TXT", buf, sizeof buf) == 6 && strcmp(buf, "inside") == 0, "read it back by path");
    CHECK(read_all("/docs//a.txt/", buf, sizeof buf) == 6, "leading, doubled and trailing slashes are fine");
    CHECK(read_all("DOCS/./../DOCS/A.TXT", buf, sizeof buf) == 6, ". and .. work");
    CHECK(read_all("A.TXT", buf, sizeof buf) == -ENOENT, "it is not in the root");
    CHECK(read_all("DOCS/A.TXT/X", buf, sizeof buf) == -ENOTDIR, "a file in the middle is -ENOTDIR");
    CHECK(read_all("NOPE/A.TXT", buf, sizeof buf) == -ENOENT, "a missing directory in the middle is -ENOENT");
    CHECK(read_all("DOCS/TOOLONGNAME.TXT", buf, sizeof buf) == -EINVAL, "a bad name in a path is -EINVAL");
    CHECK(read_all("", buf, sizeof buf) == -ENOENT, "an empty path is -ENOENT");

    int dn = diskfs_opendir(DISKFS_ROOT, "DOCS");
    CHECK(dn > 0 && diskfs_refs(dn) == 1, "opendir gives a referenced node");
    CHECK(diskfs_opendir(DISKFS_ROOT, "DOCS/A.TXT") == -ENOTDIR, "opendir of a file is -ENOTDIR");
    int fn = diskfs_lookup(dn, "a.txt");
    CHECK(fn > 0, "lookup relative to a directory node");
    diskfs_unref(fn);
    CHECK(diskfs_opendir(dn, "..") == DISKFS_ROOT && diskfs_opendir(DISKFS_ROOT, "..") == DISKFS_ROOT,
          ".. of a top directory and of the root is the root");
    CHECK(diskfs_getcwd(dn, buf, sizeof buf) == 5 && strcmp(buf, "/DOCS") == 0, "getcwd names it");
    CHECK(diskfs_getcwd(DISKFS_ROOT, buf, sizeof buf) == 1 && strcmp(buf, "/") == 0, "and the root");
    CHECK(diskfs_getcwd(dn, buf, 5) == -ENAMETOOLONG, "getcwd into a short buffer is -ENAMETOOLONG");

    // A directory grows past its first cluster.
    CHECK(diskfs_mkdir(dn, "SUB") == 0, "mkdir relative to a directory node");
    int sub = diskfs_opendir(DISKFS_ROOT, "/DOCS/SUB");
    CHECK(sub > 0 && diskfs_getcwd(sub, buf, sizeof buf) == 9 && strcmp(buf, "/DOCS/SUB") == 0,
          "a nested directory knows its path");
    int made = 0;
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "DOCS/SUB/F%d.TXT", i);
        if (diskfs_write_file(DISKFS_ROOT, name, name, (uint32_t)strlen(name)) == 0) made++;
    }
    CHECK(made == 40, "40 files go into a directory that began with room for 14");
    CHECK(count_entries(sub) == 40, "readdir lists all 40, without . and ..");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("DOCS/SUB/F39.TXT", buf, sizeof buf) == 16 && strcmp(buf, "DOCS/SUB/F39.TXT") == 0,
          "the last one reads back");

    CHECK(diskfs_rmdir(DISKFS_ROOT, "DOCS/SUB") == -EBUSY, "rmdir of a directory in use is -EBUSY");
    diskfs_unref(sub);
    CHECK(diskfs_rmdir(DISKFS_ROOT, "DOCS/SUB") == -ENOTEMPTY, "rmdir of a full one is -ENOTEMPTY");
    CHECK(diskfs_rmdir(DISKFS_ROOT, "DOCS/A.TXT") == -ENOTDIR, "rmdir of a file is -ENOTDIR");
    CHECK(diskfs_rmdir(DISKFS_ROOT, "/") == -EBUSY && diskfs_rmdir(dn, ".") == -EBUSY, "/ and . cannot go");
    CHECK(diskfs_rmdir(DISKFS_ROOT, "NOPE") == -ENOENT, "rmdir of nothing is -ENOENT");
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "F%d.TXT", i);
        int sd = diskfs_opendir(dn, "SUB");
        diskfs_unlink(sd, name);
        diskfs_unref(sd);
    }
    CHECK(diskfs_rmdir(dn, "SUB") == 0, "rmdir once it is empty");
    CHECK(diskfs_opendir(DISKFS_ROOT, "DOCS/SUB") == -ENOENT, "and it is gone");
    fn = diskfs_lookup(DISKFS_ROOT, "DOCS/A.TXT");
    CHECK(diskfs_unlink(dn, "A.TXT") == -EBUSY, "an open file in a directory cannot be deleted");
    diskfs_unref(fn);
    CHECK(diskfs_unlink(dn, "A.TXT") == 0, "until it is closed");
    diskfs_unref(dn);
    CHECK(diskfs_refs(dn) == 0 && diskfs_rmdir(DISKFS_ROOT, "DOCS") == 0, "the last reference goes, and so can DOCS");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "every cluster comes back, the grown directory's too");
    CHECK(count_entries(DISKFS_ROOT) == 0, "the root is empty again");

    // A failed write of the new directory's cluster leaves nothing.
    writes_left = 0;
    CHECK(diskfs_mkdir(DISKFS_ROOT, "BROKEN") == -EIO, "mkdir reports a disk error");
    writes_left = -1;
    CHECK(diskfs_init() == 0 && diskfs_opendir(DISKFS_ROOT, "BROKEN") == -ENOENT, "and leaves no entry");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "and no cluster");

    // A tree survives a remount.
    diskfs_mkdir(DISKFS_ROOT, "KEEP");
    diskfs_mkdir(DISKFS_ROOT, "KEEP/INNER");
    diskfs_write_file(DISKFS_ROOT, "KEEP/INNER/NOTE.TXT", "deep", 4);
    CHECK(diskfs_init() == 0, "remount");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("KEEP/INNER/NOTE.TXT", buf, sizeof buf) == 4 && strcmp(buf, "deep") == 0,
          "a file two directories down reads back");
    int inner = diskfs_opendir(DISKFS_ROOT, "KEEP/INNER");
    CHECK(diskfs_getcwd(inner, buf, sizeof buf) > 0 && strcmp(buf, "/KEEP/INNER") == 0, "and its directory's path");
    diskfs_unref(inner);

    // Nesting stops before a directory's path would not fit PATH_MAX.
    char path[512] = "DEEP";
    int depth = 0;
    while (diskfs_mkdir(DISKFS_ROOT, path) == 0 && depth < 40) {
        depth++;
        strcat(path, "/DDDDDDDD");
    }
    CHECK(diskfs_mkdir(DISKFS_ROOT, path) == -ENAMETOOLONG, "mkdir too deep is -ENAMETOOLONG");
    *strrchr(path, '/') = 0;
    int deepest = diskfs_opendir(DISKFS_ROOT, path);
    int len = diskfs_getcwd(deepest, buf, sizeof buf);
    CHECK(depth > 5 && len > 100 && len < PATH_MAX, "the deepest directory's path still fits");
    diskfs_unref(deepest);
}

static void test_rename(void) {
    char buf[256];
    format(16384, 1);
    diskfs_init();
    uint32_t clusters, free0, f, cb;
    dirent_t d;
    diskfs_write_file(DISKFS_ROOT, "A.TXT", "alpha", 5);
    diskfs_write_file(DISKFS_ROOT, "B.TXT", "beta", 4);
    diskfs_mkdir(DISKFS_ROOT, "D");
    diskfs_stats(&clusters, &free0, &cb);

    CHECK(diskfs_rename(DISKFS_ROOT, "a.txt", "c.txt") == 0, "rename a file");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("C.TXT", buf, sizeof buf) == 5 && strcmp(buf, "alpha") == 0 &&
          read_all("A.TXT", buf, sizeof buf) == -ENOENT, "the new name has the data, the old one is gone");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "B.TXT") == -EEXIST, "renaming onto a taken name is -EEXIST");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "D") == -EEXIST, "and onto a directory");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "/C.TXT") == 0, "renaming onto itself is a no-op");
    CHECK(diskfs_rename(DISKFS_ROOT, "NOPE", "X") == -ENOENT, "renaming nothing is -ENOENT");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "LONGERNAME.TXT") == -EINVAL, "to a bad name is -EINVAL");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "NOPE/C.TXT") == -ENOENT, "into a missing directory is -ENOENT");
    CHECK(diskfs_rename(DISKFS_ROOT, ".", "X") == -EBUSY && diskfs_rename(DISKFS_ROOT, "/", "X") == -EBUSY,
          ". and / cannot be renamed");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "..") == -EEXIST, "nothing can be renamed to ..");

    // Moving into a directory, with the file open the whole time.
    int fn = diskfs_lookup(DISKFS_ROOT, "C.TXT");
    CHECK(diskfs_rename(DISKFS_ROOT, "C.TXT", "D/MOVED.TXT") == 0, "move an open file into a directory");
    memset(buf, 0, sizeof buf);
    CHECK(diskfs_pread(fn, buf, sizeof buf, 0) == 5 && strcmp(buf, "alpha") == 0, "it still reads through its node");
    CHECK(diskfs_pwrite(fn, "!", 1, 5) == 1, "and writes");
    CHECK(read_all("D/MOVED.TXT", buf, sizeof buf) == 6 && memcmp(buf, "alpha!", 6) == 0,
          "and the write lands in the moved file");
    CHECK(diskfs_unlink(DISKFS_ROOT, "D/MOVED.TXT") == -EBUSY, "it is still known to be open");
    diskfs_unref(fn);
    CHECK(count_entries(DISKFS_ROOT) == 2, "the root keeps B.TXT and D");

    // Moving directories: ".." follows, and loops are refused.
    diskfs_mkdir(DISKFS_ROOT, "D/SUB");
    diskfs_write_file(DISKFS_ROOT, "D/SUB/IN.TXT", "in", 2);
    diskfs_mkdir(DISKFS_ROOT, "E");
    int sub = diskfs_opendir(DISKFS_ROOT, "D/SUB");
    CHECK(diskfs_rename(DISKFS_ROOT, "D", "D/SUB/D") == -EINVAL, "a directory cannot go inside itself");
    CHECK(diskfs_rename(DISKFS_ROOT, "D", "D/X") == -EINVAL, "or straight into itself");
    CHECK(diskfs_rename(DISKFS_ROOT, "D/SUB", "E/SUB2") == 0, "move a directory that is in use");
    CHECK(diskfs_getcwd(sub, buf, sizeof buf) > 0 && strcmp(buf, "/E/SUB2") == 0, "its node knows the new path");
    int e = diskfs_opendir(DISKFS_ROOT, "E");
    CHECK(diskfs_opendir(sub, "..") == e, "its .. points at the new parent");
    diskfs_unref(e); diskfs_unref(e);
    CHECK(read_all("E/SUB2/IN.TXT", buf, sizeof buf) == 2, "its files came along");
    CHECK(diskfs_opendir(DISKFS_ROOT, "D/SUB") == -ENOENT, "and the old place is empty");
    diskfs_unref(sub);
    CHECK(diskfs_rename(DISKFS_ROOT, "E/SUB2", "SUB3") == 0, "a directory moves up to the root");
    sub = diskfs_opendir(DISKFS_ROOT, "SUB3/..");
    CHECK(sub == DISKFS_ROOT, "and its .. is the root");
    CHECK(diskfs_init() == 0 && read_all("SUB3/IN.TXT", buf, sizeof buf) == 2, "all of it survives a remount");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0 - 3, "renames cost no clusters (only SUB, E and IN.TXT do)");

    // stat.
    CHECK(diskfs_stat(DISKFS_ROOT, "b.txt", &d) == 0 && strcmp(d.name, "B.TXT") == 0 && d.size == 4 &&
          d.flags == 0, "stat a file");
    dirent_t d2;
    CHECK(diskfs_stat(DISKFS_ROOT, "/SUB3/../B.TXT", &d2) == 0 && d2.id == d.id, "two paths to it share an id");
    CHECK(diskfs_stat(DISKFS_ROOT, "E", &d) == 0 && d.flags == DIRENT_DIR && strcmp(d.name, "E") == 0,
          "stat a directory");
    int en = diskfs_opendir(DISKFS_ROOT, "E");
    CHECK(diskfs_stat(en, ".", &d2) == 0 && d2.id == d.id && strcmp(d2.name, "E") == 0, "stat . names it");
    diskfs_unref(en);
    CHECK(diskfs_stat(DISKFS_ROOT, "/", &d) == 0 && strcmp(d.name, "/") == 0 && d.flags == DIRENT_DIR, "stat /");
    CHECK(diskfs_stat(DISKFS_ROOT, "NOPE", &d) == -ENOENT, "stat of nothing is -ENOENT");

    // A move that would push a directory's path past PATH_MAX.
    char path[256] = "DEEP";
    int depth = 0;
    while (diskfs_mkdir(DISKFS_ROOT, path) == 0 && depth < 40) {
        depth++;
        strcat(path, "/DDDDDDDD");
    }
    diskfs_mkdir(DISKFS_ROOT, "T");
    diskfs_mkdir(DISKFS_ROOT, "T/U");
    *strrchr(path, '/') = 0;
    strcat(path, "/X");
    CHECK(diskfs_rename(DISKFS_ROOT, "T", path) == -ENAMETOOLONG, "moving a tree too deep is -ENAMETOOLONG");
    *strrchr(path, '/') = 0;
    *strrchr(path, '/') = 0;
    strcat(path, "/X");
    CHECK(diskfs_rename(DISKFS_ROOT, "T/U", path) == 0, "a smaller one fits where the tree did not");
}

static void test_big_clusters(void) {
    char buf[20000];
    format(32768, 4);
    CHECK(diskfs_init() == 0, "volume with 2 KB clusters mounts");
    uint32_t clusters, f, cb;
    diskfs_stats(&clusters, &f, &cb);
    CHECK(cb == 2048, "cluster size is 2 KB");
    static char data[10000];
    for (int i = 0; i < 10000; i++) data[i] = (char)(i * 7);
    CHECK(diskfs_write_file(DISKFS_ROOT, "DATA.BIN", data, sizeof data) == 0, "10000 bytes over 5 clusters");
    CHECK(diskfs_init() == 0 && read_all("DATA.BIN", buf, sizeof buf) == 10000 && memcmp(buf, data, 10000) == 0,
          "reads back after a remount");
    diskfs_write_file(DISKFS_ROOT, "KEEP.TXT", "kept\n", 5);

    // Directories on 2 KB clusters, one grown past a cluster, for fsck.
    CHECK(diskfs_mkdir(DISKFS_ROOT, "TREE") == 0 && diskfs_mkdir(DISKFS_ROOT, "TREE/LEAF") == 0,
          "directories on 2 KB clusters");
    int made = 0;
    for (int i = 0; i < 70; i++) {
        char name[32];
        snprintf(name, sizeof name, "TREE/LEAF/F%d.TXT", i);
        if (diskfs_write_file(DISKFS_ROOT, name, "leaf\n", 5) == 0) made++;
    }
    int leaf = diskfs_init() == 0 ? diskfs_opendir(DISKFS_ROOT, "TREE/LEAF") : -1;
    CHECK(made == 70 && leaf > 0 && count_entries(leaf) == 70, "70 files in one, past its first 64 slots");
    diskfs_unref(leaf);
    // fsck checks the ".." of a moved directory, and a renamed file.
    CHECK(diskfs_mkdir(DISKFS_ROOT, "OTHER") == 0 && diskfs_rename(DISKFS_ROOT, "TREE/LEAF", "OTHER/LEAF") == 0 &&
          diskfs_rename(DISKFS_ROOT, "OTHER/LEAF/F3.TXT", "/F3MOVED.TXT") == 0, "moves on 2 KB clusters");
}

static void test_bad_volumes(void) {
    format(16384, 1);
    disk[510] = 0;
    CHECK(diskfs_init() == -EINVAL, "missing boot signature is refused");
    format(16384, 1);
    disk[13] = 3;
    CHECK(diskfs_init() == -EINVAL, "cluster size that is not a power of two is refused");
    format(16384, 1);
    put16(disk + 19, 20000);
    CHECK(diskfs_init() == -EINVAL, "volume bigger than the disk is refused");
    format(16384, 1);
    put16(disk + 22, 2);
    CHECK(diskfs_init() == -EINVAL, "FAT too small for the clusters is refused");
    format(4096, 1);  // 4000-odd clusters: FAT12 by definition
    CHECK(diskfs_init() == -EINVAL, "FAT12-sized volume is refused");
    CHECK(!diskfs_ready(), "and nothing is mounted");
    disk_sectors = 0;
    CHECK(diskfs_init() == -EIO, "no disk at all is -EIO");
}

int main(void) {
    test_heap();
    heap_init(heap_area, sizeof heap_area);
    test_names();
    test_basic();
    test_full_disk();
    test_io_errors();
    test_bad_volumes();
    test_dirs();
    test_rename();
    test_big_clusters();  // last: its image is what fsck checks

    FILE* img = fopen("test_disk.img", "wb");
    if (img) {
        fwrite(disk, 512, disk_sectors, img);
        fclose(img);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

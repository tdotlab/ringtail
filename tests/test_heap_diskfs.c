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
    int node = diskfs_lookup(name);
    if (node < 0) return node;
    return diskfs_pread(node, buf, max, 0);
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
    CHECK(diskfs_readdir(&cursor, buf, 0) == -ENOENT, "the volume label is not listed as a file");
    CHECK(diskfs_lookup("HELLO.TXT") == -ENOENT, "missing file is not found");
    CHECK(diskfs_lookup("BAD NAME") == -EINVAL, "lookup of an invalid name is -EINVAL");

    int node = diskfs_create("hello.txt");
    CHECK(node >= 0, "create succeeds");
    CHECK(diskfs_create("HELLO.TXT") == -EEXIST, "create is case-insensitive and refuses duplicates");
    CHECK(diskfs_lookup("Hello.Txt") == node, "lookup is case-insensitive");
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

    CHECK(diskfs_write_file("notes.txt", "first", 5) == 0, "write_file creates a file");
    CHECK(diskfs_write_file("notes.txt", "second version", 14) == 0, "write_file replaces it");
    memset(buf, 0, sizeof buf);
    CHECK(read_all("NOTES.TXT", buf, sizeof buf) == 14 && strcmp(buf, "second version") == 0, "the new contents are there");
    CHECK(diskfs_write_file("notes.txt", "", 0) == 0 && read_all("NOTES.TXT", buf, sizeof buf) == 0,
          "write_file with no data empties the file");

    // Open files cannot be deleted.
    int n2 = diskfs_lookup("NOTES.TXT");
    diskfs_ref(n2);
    CHECK(diskfs_refs(n2) == 1, "ref counts");
    CHECK(diskfs_unlink("NOTES.TXT") == -EBUSY, "unlink of an open file is -EBUSY");
    diskfs_unref(n2);
    CHECK(diskfs_unlink("NOTES.TXT") == 0, "unlink once closed");
    CHECK(diskfs_lookup("NOTES.TXT") == -ENOENT, "it is gone");
    CHECK(diskfs_unlink("NOTES.TXT") == -ENOENT, "second unlink is -ENOENT");

    // Directory listing.
    diskfs_write_file("A.TXT", "a", 1);
    diskfs_write_file("B.TXT", "bb", 2);
    int count = 0, saw_b = 0;
    char name[FS_NAME_MAX];
    uint32_t size;
    cursor = 0;
    while (diskfs_readdir(&cursor, name, &size) == 0) {
        count++;
        if (strcmp(name, "B.TXT") == 0 && size == 2) saw_b = 1;
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
    diskfs_write_file("KEEP.TXT", "keep me", 7);
    diskfs_stats(&clusters, &free0, &cb);

    static uint8_t huge[9 * 1024 * 1024];
    memset(huge, 'Z', sizeof huge);
    CHECK(diskfs_write_file("HUGE.BIN", huge, sizeof huge) == -ENOSPC, "write bigger than the disk is -ENOSPC");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "a failed write_file gives back every cluster it took");
    CHECK(diskfs_lookup("HUGE.BIN") == -ENOENT, "and leaves no half-made file");

    int node = diskfs_create("FILL.BIN");
    int r = diskfs_pwrite(node, huge, sizeof huge, 0);
    CHECK(r > 0 && (uint32_t)r < sizeof huge, "pwrite to a full disk writes what fits");
    CHECK(diskfs_size(node) == (uint32_t)r, "and the size says how much");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == 0, "the disk is now full");
    CHECK(diskfs_pwrite(node, "more", 4, (uint32_t)r) == -ENOSPC, "further growth is -ENOSPC");
    CHECK(diskfs_create("EMPTY.TXT") >= 0, "empty files still fit on a full disk");
    CHECK(diskfs_unlink("FILL.BIN") == 0, "deleting the filler");
    diskfs_stats(&clusters, &f, &cb);
    CHECK(f == free0, "frees all of it");
    char buf[16] = {0};
    CHECK(read_all("KEEP.TXT", buf, sizeof buf) == 7 && strcmp(buf, "keep me") == 0, "the other file is untouched");
}

static void test_io_errors(void) {
    char buf[64];
    format(16384, 1);
    diskfs_init();
    diskfs_write_file("SAFE.TXT", "original", 8);

    // Fail the very first write: the new data never lands, so the
    // directory must still point at the old data.
    writes_left = 0;
    CHECK(diskfs_write_file("SAFE.TXT", "replacement", 11) == -EIO, "disk error is reported");
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
        if (diskfs_create(name) >= 0) made++;
    }
    CHECK(made == 512 - 2, "the root directory fills at its entry count");
    CHECK(diskfs_create("ONEMORE.TXT") == -ENOSPC, "and then create is -ENOSPC");
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
    CHECK(diskfs_write_file("DATA.BIN", data, sizeof data) == 0, "10000 bytes over 5 clusters");
    CHECK(diskfs_init() == 0 && read_all("DATA.BIN", buf, sizeof buf) == 10000 && memcmp(buf, data, 10000) == 0,
          "reads back after a remount");
    diskfs_write_file("KEEP.TXT", "kept\n", 5);
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
    test_big_clusters();  // last: its image is what fsck checks

    FILE* img = fopen("test_disk.img", "wb");
    if (img) {
        fwrite(disk, 512, disk_sectors, img);
        fclose(img);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

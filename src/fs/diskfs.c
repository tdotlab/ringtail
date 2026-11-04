// v0.10.0: disk filesystem, now FAT16, the standard layout.
// A filesystem is just how an OS organizes files on a disk:
// where each file starts, how big it is, which pieces belong
// to it, and what is free. RTFS, the previous filesystem, was
// a homemade flat layout: a fixed directory, a bitmap, and
// each file in one unbroken run of sectors. It worked, and it
// was small enough to hold in mind, but no other computer
// could read it. FAT is the standard alternative, used by DOS,
// old Windows, USB sticks, and SD cards, and its name is
// literal: File Allocation Table.
// How FAT works, in plain terms. The disk is split into equal
// chunks called clusters (here, one sector each in our own
// images, though the kernel accepts any power-of-two cluster
// size a real tool might choose). Near the start of the disk
// sits a table, one entry per cluster, saying what that
// cluster holds: 0 means free, a cluster number means "the
// next piece of this file is that cluster", and a special
// end value means "this is the last piece". A file is a chain
// followed through that table, so its pieces do not have to
// sit next to each other: deleting a file in the middle
// leaves holes, and a new file can fill those holes, which
// RTFS could not do (it needed one unbroken run, and failed
// when none fit, even with free space scattered around).
// Layout, in order. Sector 0 is the boot sector with a BPB,
// a BIOS Parameter Block: a small table saying how big a
// sector is, how many sectors per cluster, how many reserved
// sectors, how many copies of the FAT, how many root
// directory entries, how many sectors the disk has in total,
// and how big one FAT is. The kernel reads that table at
// init and works out everything else from it, instead of
// hardcoding RTFS-style constants, so it can read disks made
// by real tools, not just disks made by its own packing tool.
// After the reserved sectors come the FAT copies (two, kept
// in sync, the standard redundancy), then the root directory
// (a fixed table, 512 entries in our images, 32 bytes each),
// then the data area, where cluster 2 is the first data
// cluster (clusters 0 and 1 are reserved, by convention).
// Names are the DOS 8.3 quirk: at most 8 characters, then a
// dot, then at most 3 for the extension, stored uppercase,
// padded with spaces, with no dot stored at all (the dot is
// implied between byte 8 and byte 9). "demo.txt" is stored
// as "DEMO    TXT". The kernel converts user names to that
// form on every operation, so typing lowercase still finds
// the file, and converts back for listing, so ls shows
// "DEMO.TXT". Names that do not fit 8.3, or use characters
// FAT forbids, are rejected, which is a visible change from
// RTFS (which allowed 31 characters, any case).
// What this version does not do, on purpose. No long file
// names (the extra entries real tools put before a short
// entry are skipped, not understood, so a file made elsewhere
// with a long name is reached by its short name, like
// LONGFI~1.TXT, and deleting it leaves its long-name entries
// orphaned in the directory, harmless but wasted slots).
// No subdirectories: only the root directory is used, like
// RTFS, so ls, cat, and run all work on root files only.
// No timestamps: directory date and time fields are left
// zero, because there is no clock yet to fill them honestly.
// No journaling: like RTFS, a crash mid-write can leave the
// FAT and directory disagreeing, and there is no log to
// replay. FAT16 only: the cluster count decides the type,
// per the spec, and counts below 4085 (FAT12) or at or above
// 65525 (FAT32) are rejected, with a clear message, instead
// of being misread. The 64 KB whole-file cap from the loader
// stays: FAT itself has no such cap, but read_all still
// refuses files over 64 KB, so programs stay tiny for now.
// The public API is unchanged from RTFS, on purpose: the
// same diskfs_* names, the same return codes, the same
// whole-file overwrite semantics (allocate a new chain,
// write it, then free the old chain, so a failed write
// leaves the old file intact). That is what makes this a
// contained swap: the ATA driver, the loader, the syscalls,
// and the shell all call the same functions, and only this
// file, the packing tool, and the version strings change.
// The FAT and the root directory live in RAM after init
// (loaded from disk once) and are written back to disk on
// every mutation, like RTFS did with its directory and
// bitmap. Data sectors are never cached: they are read from
// and written to disk on each operation. All public
// functions run under interrupt save/restore, so a timer
// switch cannot catch a FAT or directory update half done.
// The sector buffer is a single heap allocation reused by
// every operation, which is safe under that lock.

#include <stdint.h>

#define SECTOR_SIZE 512
#define FAT_EOC_MIN 0xFFF8
#define FAT_BAD 0xFFF7
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0F

// BPB offsets in sector 0.
#define BPB_BYTES_PER_SECTOR 11
#define BPB_SECTORS_PER_CLUSTER 13
#define BPB_RESERVED 14
#define BPB_NUM_FATS 16
#define BPB_ROOT_ENTRIES 17
#define BPB_TOTAL16 19
#define BPB_MEDIA 21
#define BPB_FAT_SIZE16 22
#define BPB_TOTAL32 32
// Directory entry offsets (32 bytes).
#define DIR_NAME 0
#define DIR_ATTR 11
#define DIR_FIRST_HIGH 20
#define DIR_FIRST_LOW 26
#define DIR_SIZE 28

extern int ata_read_sector(uint32_t lba, uint8_t* buf);
extern int ata_write_sector(uint32_t lba, const uint8_t* buf);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern void print(const char* s);
extern void print_char(char c);

static int fs_ok = 0;
static uint32_t sectors_per_cluster = 0;
static uint32_t cluster_bytes = 0;
static uint32_t reserved_sectors = 0;
static uint32_t num_fats = 0;
static uint32_t root_entries = 0;
static uint32_t total_sectors = 0;
static uint32_t fat_size_sectors = 0;
static uint32_t fat_start = 0;
static uint32_t root_start = 0;
static uint32_t root_sectors = 0;
static uint32_t data_start = 0;
static uint32_t cluster_count = 0;
static uint8_t* fat_buf = 0;
static uint8_t* root_buf = 0;
static uint8_t* sector_buf = 0;

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { print_char('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) print_char(buf[--i]);
}

#ifdef RINGTAIL_KERNEL
static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    return flags;
}
static void irq_restore(uint32_t flags) {
    if (flags & 0x200) __asm__ volatile("sti");
}
#else
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t flags) { (void)flags; }
#endif

static uint16_t get_u16(const uint8_t* p, uint32_t off) {
    return (uint16_t)p[off] | ((uint16_t)p[off + 1] << 8);
}

static uint32_t get_u32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static void put_u16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
}

static void put_u32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
    p[off + 2] = (v >> 16) & 0xFF;
    p[off + 3] = (v >> 24) & 0xFF;
}

static int is_power_of_two(uint32_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

// Convert a user name to the 11-byte FAT raw form (8 + 3,
// uppercase, space-padded, no dot). Returns 0 on success,
// -1 if the name does not fit 8.3 or uses a forbidden
// character. Lowercase is uppercased, so lookup is
// case-insensitive; the raw form is what directory entries
// are compared against.
static int name_to_raw(const char* name, uint8_t raw[11]) {
    if (!name || name[0] == 0) return -1;
    const char* dot = 0;
    int dots = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') { dots++; dot = name + i; }
    }
    if (dots > 1) return -1;
    int base_len = dot ? (int)(dot - name) : my_strlen(name);
    int ext_len = dot ? my_strlen(dot + 1) : 0;
    if (base_len < 1 || base_len > 8 || ext_len > 3) return -1;
    for (int i = 0; i < 11; i++) raw[i] = ' ';
    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (c < 0x20 || c == 0x7F) return -1;
        if (c == ' ' || c == '"' || c == '*' || c == '+' || c == ',' ||
            c == '/' || c == ':' || c == ';' || c == '<' || c == '=' ||
            c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' ||
            c == '|') return -1;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        raw[i] = (uint8_t)c;
    }
    for (int i = 0; i < ext_len; i++) {
        char c = dot[1 + i];
        if (c < 0x20 || c == 0x7F) return -1;
        if (c == ' ' || c == '"' || c == '*' || c == '+' || c == ',' ||
            c == '/' || c == ':' || c == ';' || c == '<' || c == '=' ||
            c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' ||
            c == '|') return -1;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        raw[8 + i] = (uint8_t)c;
    }
    return 0;
}

// Convert a raw 11-byte name back to display form
// ("DEMO    TXT" -> "DEMO.TXT", "WELCOME     " -> "WELCOME").
// out must hold 13 bytes (8 + 1 + 3 + null).
static void raw_to_display(const uint8_t raw[11], char out[13]) {
    int bi = 8;
    while (bi > 0 && raw[bi - 1] == ' ') bi--;
    int ei = 3;
    while (ei > 0 && raw[8 + ei - 1] == ' ') ei--;
    int o = 0;
    for (int i = 0; i < bi; i++) out[o++] = (char)raw[i];
    if (ei > 0) {
        out[o++] = '.';
        for (int i = 0; i < ei; i++) out[o++] = (char)raw[8 + i];
    }
    out[o] = 0;
}

static int raw_equal(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint8_t* entry_at(uint32_t i) {
    return root_buf + i * 32;
}

static uint16_t fat_get(uint32_t cluster) {
    return get_u16(fat_buf, cluster * 2);
}

static void fat_set(uint32_t cluster, uint16_t v) {
    put_u16(fat_buf, cluster * 2, v);
}

static int fat_is_eoc(uint16_t v) {
    return v >= FAT_EOC_MIN;
}

static uint32_t cluster_lba(uint32_t cluster) {
    return data_start + (cluster - 2) * sectors_per_cluster;
}

static int write_back_fat(void) {
    for (uint32_t f = 0; f < num_fats; f++) {
        for (uint32_t s = 0; s < fat_size_sectors; s++) {
            for (int i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = fat_buf[s * SECTOR_SIZE + i];
            if (ata_write_sector(fat_start + f * fat_size_sectors + s, sector_buf) != 0) return -1;
        }
    }
    return 0;
}

static int write_back_root(void) {
    for (uint32_t s = 0; s < root_sectors; s++) {
        for (int i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = root_buf[s * SECTOR_SIZE + i];
        if (ata_write_sector(root_start + s, sector_buf) != 0) return -1;
    }
    return 0;
}

// Index of the directory entry for a regular file with this
// raw name, or -1. Skips deleted entries, long-name entries,
// directories, and volume labels, and stops at the first
// never-used entry, per the FAT end-of-directory rule.
static int find_file_raw(const uint8_t raw[11]) {
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[DIR_ATTR] == ATTR_LFN) continue;
        if (e[DIR_ATTR] & ATTR_DIRECTORY) continue;
        if (e[DIR_ATTR] & ATTR_VOLUME) continue;
        if (get_u16(e, DIR_FIRST_HIGH) != 0) continue;
        if (raw_equal(e, raw)) return (int)i;
    }
    return -1;
}

// True if any live directory entry (file, directory, or
// volume label, but not a long-name entry) already uses this
// raw name. Used by create to reject a file colliding with
// a directory of the same name, which find_file_raw alone
// would miss.
static int name_in_use_raw(const uint8_t raw[11]) {
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[DIR_ATTR] == ATTR_LFN) continue;
        if (raw_equal(e, raw)) return 1;
    }
    return 0;
}

static int free_slot(void) {
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00 || e[0] == 0xE5) return (int)i;
    }
    return -1;
}

// Allocate a chain of n clusters, linked in RAM, last marked
// end-of-chain. On failure, any partially allocated chain is
// freed again in RAM, so the caller can return without having
// changed anything on disk (the FAT has not been written
// back yet). Returns 0 on success with *first set, -1 on
// failure. n == 0 means an empty file: *first = 0.
static int alloc_chain(uint32_t n, uint32_t* first_out) {
    if (n == 0) { *first_out = 0; return 0; }
    uint32_t first = 0, prev = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t found = 0;
        for (uint32_t c = 2; c < cluster_count + 2; c++) {
            if (fat_get(c) == 0) { found = c; break; }
        }
        if (!found) {
            if (first) {
                uint32_t cur = first;
                while (cur >= 2 && cur < cluster_count + 2) {
                    uint16_t nxt = fat_get(cur);
                    fat_set(cur, 0);
                    if (fat_is_eoc(nxt)) break;
                    cur = nxt;
                }
            }
            return -1;
        }
        fat_set(found, 0xFFFF);
        if (prev) fat_set(prev, (uint16_t)found);
        else first = found;
        prev = found;
    }
    *first_out = first;
    return 0;
}

// Free a chain in RAM, stopping at end-of-chain or at a
// corrupt link, with a step bound so a looped chain cannot
// hang the operation.
static void free_chain(uint32_t first) {
    if (first < 2 || first >= cluster_count + 2) return;
    uint32_t cur = first;
    for (uint32_t steps = 0; steps <= cluster_count + 1; steps++) {
        if (cur < 2 || cur >= cluster_count + 2) return;
        uint16_t nxt = fat_get(cur);
        fat_set(cur, 0);
        if (fat_is_eoc(nxt)) return;
        cur = nxt;
    }
}

// The cluster at position index in a chain (0 = first), or
// 0 if the chain ends early or is corrupt.
static uint32_t cluster_at(uint32_t first, uint32_t index) {
    uint32_t cur = first;
    for (uint32_t i = 0; i < index; i++) {
        if (cur < 2 || cur >= cluster_count + 2) return 0;
        uint16_t nxt = fat_get(cur);
        if (fat_is_eoc(nxt)) return 0;
        cur = nxt;
    }
    if (cur < 2 || cur >= cluster_count + 2) return 0;
    return cur;
}

void diskfs_init(void) {
    // A second init (the host test does this for persistence)
    // frees the old buffers first, instead of leaking them
    // the way RTFS did.
    if (sector_buf) { kfree(sector_buf); sector_buf = 0; }
    if (fat_buf) { kfree(fat_buf); fat_buf = 0; }
    if (root_buf) { kfree(root_buf); root_buf = 0; }
    fs_ok = 0;
    sector_buf = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (!sector_buf) { print("Disk: out of memory, files unavailable\n"); return; }
    if (ata_read_sector(0, sector_buf) != 0) {
        print("Disk: not found, files unavailable\n");
        return;
    }
    // Parse the BPB. Everything else is derived from it, so
    // a disk made by a real tool works, not just our own.
    uint32_t bps = get_u16(sector_buf, BPB_BYTES_PER_SECTOR);
    uint32_t spc = sector_buf[BPB_SECTORS_PER_CLUSTER];
    uint32_t res = get_u16(sector_buf, BPB_RESERVED);
    uint32_t nfats = sector_buf[BPB_NUM_FATS];
    uint32_t rent = get_u16(sector_buf, BPB_ROOT_ENTRIES);
    uint32_t tot16 = get_u16(sector_buf, BPB_TOTAL16);
    uint32_t fsz = get_u16(sector_buf, BPB_FAT_SIZE16);
    uint32_t tot32 = get_u32(sector_buf, BPB_TOTAL32);
    uint32_t tot = tot16 != 0 ? tot16 : tot32;
    int sig_ok = sector_buf[510] == 0x55 && sector_buf[511] == 0xAA;
    if (bps != SECTOR_SIZE || !is_power_of_two(spc) || spc == 0 || spc > 128 ||
        res == 0 || nfats == 0 || rent == 0 || tot == 0 || fsz == 0 || !sig_ok) {
        print("Disk: no FAT filesystem, files unavailable\n");
        return;
    }
    uint32_t rsec = (rent * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t fstart = res;
    uint32_t rstart = fstart + nfats * fsz;
    uint32_t dstart = rstart + rsec;
    if (dstart >= tot) {
        print("Disk: no FAT filesystem, files unavailable\n");
        return;
    }
    uint32_t dsec = tot - dstart;
    uint32_t ccount = dsec / spc;
    // The cluster count decides the FAT type, per the spec.
    // FAT16 is 4085..65524 clusters; anything else is FAT12
    // or FAT32, which this version does not read.
    if (ccount < 4085 || ccount >= 65525) {
        print("Disk: not a FAT16 filesystem, files unavailable\n");
        return;
    }
    sectors_per_cluster = spc;
    cluster_bytes = spc * SECTOR_SIZE;
    reserved_sectors = res;
    num_fats = nfats;
    root_entries = rent;
    total_sectors = tot;
    fat_size_sectors = fsz;
    fat_start = fstart;
    root_start = rstart;
    root_sectors = rsec;
    data_start = dstart;
    cluster_count = ccount;
    fat_buf = (uint8_t*)kmalloc(fsz * SECTOR_SIZE);
    root_buf = (uint8_t*)kmalloc(rsec * SECTOR_SIZE);
    if (!fat_buf || !root_buf) {
        print("Disk: out of memory, files unavailable\n");
        return;
    }
    for (uint32_t s = 0; s < fsz; s++) {
        if (ata_read_sector(fat_start + s, sector_buf) != 0) {
            print("Disk: read error, files unavailable\n");
            return;
        }
        for (int i = 0; i < SECTOR_SIZE; i++) fat_buf[s * SECTOR_SIZE + i] = sector_buf[i];
    }
    for (uint32_t s = 0; s < rsec; s++) {
        if (ata_read_sector(root_start + s, sector_buf) != 0) {
            print("Disk: read error, files unavailable\n");
            return;
        }
        for (int i = 0; i < SECTOR_SIZE; i++) root_buf[s * SECTOR_SIZE + i] = sector_buf[i];
    }
    fs_ok = 1;
    int count = 0;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[DIR_ATTR] == ATTR_LFN) continue;
        if (e[DIR_ATTR] & ATTR_DIRECTORY) continue;
        if (e[DIR_ATTR] & ATTR_VOLUME) continue;
        count++;
    }
    print("Disk: FAT16, ");
    print_num(count);
    print(" files\n");
}

int diskfs_create(const char* name) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = 0;
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) result = -2;
    else if (name_in_use_raw(raw)) result = -1;
    else {
        int slot = free_slot();
        if (slot < 0) result = -3;
        else {
            uint8_t* e = entry_at((uint32_t)slot);
            for (int i = 0; i < 32; i++) e[i] = 0;
            for (int i = 0; i < 11; i++) e[i] = raw[i];
            e[DIR_ATTR] = ATTR_ARCHIVE;
            put_u16(e, DIR_FIRST_LOW, 0);
            put_u32(e, DIR_SIZE, 0);
            if (write_back_root() != 0) {
                e[0] = 0x00;
                result = -3;
            }
        }
    }
    irq_restore(flags);
    return result;
}

// Length-based whole-file write, the real implementation.
// Same semantics as RTFS: this overwrites the whole file.
// The syscall path builds the new full content (old bytes
// plus the written slice) and calls this, so offset writes
// work through it without this function needing an offset.
// Allocate a new chain first, write it, then free the old
// chain: a failed write leaves the old file intact on disk,
// because the FAT and directory have not been written back
// yet when the failure is detected.
int diskfs_write_bytes(const char* name, const uint8_t* content, uint32_t len) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = 0;
    uint8_t raw[11];
    int idx = -1;
    int auto_created = 0;
    if (name_to_raw(name, raw) != 0) result = -2;
    else {
        idx = find_file_raw(raw);
        if (idx < 0) {
            if (name_in_use_raw(raw)) result = -1;
            else {
                int slot = free_slot();
                if (slot < 0) result = -3;
                else {
                    uint8_t* e = entry_at((uint32_t)slot);
                    for (int i = 0; i < 32; i++) e[i] = 0;
                    for (int i = 0; i < 11; i++) e[i] = raw[i];
                    e[DIR_ATTR] = ATTR_ARCHIVE;
                    idx = slot;
                    auto_created = 1;
                }
            }
        }
    }
    if (result == 0) {
        uint32_t needed = (len + cluster_bytes - 1) / cluster_bytes;
        uint32_t new_first = 0;
        if (alloc_chain(needed, &new_first) != 0) {
            if (auto_created) entry_at((uint32_t)idx)[0] = 0x00;
            result = -2;
        } else {
            int werr = 0;
            uint32_t cur = new_first;
            for (uint32_t ci = 0; ci < needed && !werr; ci++) {
                for (uint32_t sec = 0; sec < sectors_per_cluster && !werr; sec++) {
                    for (int i = 0; i < SECTOR_SIZE; i++) {
                        uint32_t off = ci * cluster_bytes + sec * SECTOR_SIZE + (uint32_t)i;
                        sector_buf[i] = off < len ? (uint8_t)content[off] : 0;
                    }
                    if (ata_write_sector(cluster_lba(cur) + sec, sector_buf) != 0) werr = 1;
                }
                if (!werr && ci + 1 < needed) {
                    uint16_t nxt = fat_get(cur);
                    if (fat_is_eoc(nxt)) werr = 1;
                    else cur = nxt;
                }
            }
            if (werr) {
                free_chain(new_first);
                if (auto_created) entry_at((uint32_t)idx)[0] = 0x00;
                result = -2;
            } else {
                uint8_t* e = entry_at((uint32_t)idx);
                uint32_t old_first = get_u16(e, DIR_FIRST_LOW);
                if (old_first != 0) free_chain(old_first);
                put_u16(e, DIR_FIRST_HIGH, 0);
                put_u16(e, DIR_FIRST_LOW, (uint16_t)new_first);
                put_u32(e, DIR_SIZE, len);
                if (write_back_fat() != 0 || write_back_root() != 0) result = -2;
            }
        }
    }
    irq_restore(flags);
    return result;
}

int diskfs_write(const char* name, const char* content) {
    return diskfs_write_bytes(name, (const uint8_t*)content, (uint32_t)my_strlen(content));
}

int diskfs_read(const char* name, char* out, uint32_t max) {
    if (!fs_ok) return -4;
    if (max == 0) return -1;
    uint32_t flags = irq_save();
    int result = -1;
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) result = -1;
    else {
        int idx = find_file_raw(raw);
        if (idx < 0) result = -1;
        else {
            uint8_t* e = entry_at((uint32_t)idx);
            uint32_t size = get_u32(e, DIR_SIZE);
            uint32_t first = get_u16(e, DIR_FIRST_LOW);
            if (size == 0) {
                out[0] = 0;
                result = 0;
            } else {
                uint32_t to_copy = size;
                if (to_copy >= max) to_copy = max - 1;
                uint32_t copied = 0;
                int rerr = 0;
                while (copied < to_copy && !rerr) {
                    uint32_t ci = copied / cluster_bytes;
                    uint32_t sec = (copied / SECTOR_SIZE) % sectors_per_cluster;
                    uint32_t cl = cluster_at(first, ci);
                    if (cl == 0) { rerr = 1; break; }
                    if (ata_read_sector(cluster_lba(cl) + sec, sector_buf) != 0) { rerr = 1; break; }
                    uint32_t off = copied % SECTOR_SIZE;
                    while (off < SECTOR_SIZE && copied < to_copy) out[copied++] = (char)sector_buf[off++];
                }
                if (rerr) result = -1;
                else {
                    out[copied] = 0;
                    result = (int)copied;
                }
            }
        }
    }
    irq_restore(flags);
    return result;
}

int diskfs_delete(const char* name) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = -1;
    uint8_t raw[11];
    if (name_to_raw(name, raw) == 0) {
        int idx = find_file_raw(raw);
        if (idx >= 0) {
            uint8_t* e = entry_at((uint32_t)idx);
            uint32_t first = get_u16(e, DIR_FIRST_LOW);
            if (first != 0) free_chain(first);
            e[0] = 0xE5;
            if (write_back_fat() != 0 || write_back_root() != 0) result = -1;
            else result = 0;
        }
    }
    irq_restore(flags);
    return result;
}

void diskfs_list(void) {
    if (!fs_ok) { print("no disk\n"); return; }
    uint32_t flags = irq_save();
    int any = 0;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[DIR_ATTR] == ATTR_LFN) continue;
        if (e[DIR_ATTR] & ATTR_DIRECTORY) continue;
        if (e[DIR_ATTR] & ATTR_VOLUME) continue;
        if (get_u16(e, DIR_FIRST_HIGH) != 0) continue;
        char disp[13];
        raw_to_display(e, disp);
        any = 1;
        print(disp);
        print(" (");
        print_num(get_u32(e, DIR_SIZE));
        print(" bytes)\n");
    }
    if (!any) print("empty\n");
    irq_restore(flags);
}

// The Nth regular file's display name, for the LIST syscall.
// Index counts regular files only, in root directory order,
// skipping deleted, long-name, directory, and volume entries,
// and entries with a nonzero first-cluster-high (not valid
// FAT16), exactly as diskfs_list does, so LIST and ls agree.
// Returns 0 on success with name_out set (the caller must
// provide at least 13 bytes: 12 for the longest 8.3 name
// plus the null terminator), or -1 if index is out of range
// (negative, or past the last file), name_out is null, or
// there is no disk. No disk I/O beyond the root directory
// already in RAM, so this is cheap to call in a loop.
int diskfs_list_at(int index, char* name_out) {
    if (!name_out) return -1;
    if (!fs_ok) return -1;
    if (index < 0) return -1;
    uint32_t flags = irq_save();
    int result = -1;
    int seen = -1;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry_at(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[DIR_ATTR] == ATTR_LFN) continue;
        if (e[DIR_ATTR] & ATTR_DIRECTORY) continue;
        if (e[DIR_ATTR] & ATTR_VOLUME) continue;
        if (get_u16(e, DIR_FIRST_HIGH) != 0) continue;
        seen++;
        if (seen == index) {
            raw_to_display(e, name_out);
            result = 0;
            break;
        }
    }
    irq_restore(flags);
    return result;
}

int diskfs_exists(const char* name) {
    if (!fs_ok) return 0;
    uint32_t flags = irq_save();
    uint8_t raw[11];
    int result = 0;
    if (name_to_raw(name, raw) == 0) result = find_file_raw(raw) >= 0;
    irq_restore(flags);
    return result;
}

// File size, for seek from the end. Returns 0 on success with
// *size_out set, -1 if the file does not exist or the name is
// invalid, -4 if there is no disk. Unlike read_all, this has
// no 64 KB cap: it only reads the directory entry, which is
// already in RAM, so it works for larger files too, and it
// does no disk I/O at all, which is what makes seek from the
// end cheap. That is why seek from the end uses this instead
// of reading the whole file just to learn its size.
int diskfs_size(const char* name, uint32_t* size_out) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = -1;
    uint8_t raw[11];
    if (name_to_raw(name, raw) == 0) {
        int idx = find_file_raw(raw);
        if (idx >= 0) {
            *size_out = get_u32(entry_at((uint32_t)idx), DIR_SIZE);
            result = 0;
        }
    }
    irq_restore(flags);
    return result;
}

// Loader support: read a whole file into a heap buffer.
// Same cap and contract as RTFS: 64 KB max, caller frees
// with kfree, 0 on failure.
uint8_t* diskfs_read_all(const char* name, uint32_t* size_out) {
    if (!fs_ok) return 0;
    uint32_t flags = irq_save();
    uint8_t* buf = 0;
    uint8_t raw[11];
    if (name_to_raw(name, raw) == 0) {
        int idx = find_file_raw(raw);
        if (idx >= 0) {
            uint8_t* e = entry_at((uint32_t)idx);
            uint32_t size = get_u32(e, DIR_SIZE);
            uint32_t first = get_u16(e, DIR_FIRST_LOW);
            if (size <= 65536) {
                buf = (uint8_t*)kmalloc(size > 0 ? size : 1);
                if (buf) {
                    uint32_t copied = 0;
                    int rerr = 0;
                    while (copied < size && !rerr) {
                        uint32_t ci = copied / cluster_bytes;
                        uint32_t sec = (copied / SECTOR_SIZE) % sectors_per_cluster;
                        uint32_t cl = cluster_at(first, ci);
                        if (cl == 0) { rerr = 1; break; }
                        if (ata_read_sector(cluster_lba(cl) + sec, sector_buf) != 0) { rerr = 1; break; }
                        uint32_t off = copied % SECTOR_SIZE;
                        while (off < SECTOR_SIZE && copied < size) buf[copied++] = sector_buf[off++];
                    }
                    if (rerr) {
                        kfree(buf);
                        buf = 0;
                    } else {
                        *size_out = size;
                    }
                }
            }
        }
    }
    irq_restore(flags);
    return buf;
}

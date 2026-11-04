// v0.7.0: disk filesystem, the flat layout from the design note.
// A disk is sectors, 512 bytes each, numbered from 0. This
// filesystem uses three regions, in order. Sector 0 is the
// superblock: magic "RTFS", version, total sectors, and where
// the other two regions start and how big they are. Next comes
// a fixed directory table (64 entries in the standard image):
// each entry is a name (up to 31 characters), a start sector, a
// size in bytes, and a used flag, 48 bytes in all. Next comes a
// bitmap, one bit per data sector, saying whether that sector
// belongs to a file. The rest is data, where each file sits as
// one contiguous run of sectors. No chains, no clusters, no
// subdirectories: to read a file you find its entry, then read
// its sectors in order. To write one you find a contiguous run
// of free sectors in the bitmap, write the data, then write the
// directory entry. To delete one you clear its bitmap bits and
// its entry. That is the whole filesystem, and that smallness
// is the point: every byte of it can be held in mind at once,
// which is what you want when it shares a version with paging
// and a loader. The real-world alternative, FAT, with its chains
// and cluster allocation, is a later piece, not this one.
// The directory and bitmap live in RAM after init (loaded from
// disk once) and are written back to disk on every mutation, so
// a file created in one boot is still there in the next, as
// long as the same disk image is attached. Data sectors are
// never cached: they are read from and written to disk on each
// operation. All public functions run under interrupt
// save/restore, like ramfs before them, so a timer switch cannot
// catch a bitmap or directory update half done. The sector
// buffer is a single heap allocation reused by every operation,
// which is safe under that lock: no two operations overlap.
// If no disk is attached, or the disk has no RTFS magic, init
// reports "no disk" and every operation fails cleanly instead
// of hanging: the OS still boots, the shell still works, only
// file commands and disk program loading are unavailable.

#include <stdint.h>

#define SECTOR_SIZE 512
#define DIR_ENTRY_SIZE 48
#define MAX_NAME 32

typedef struct {
    char name[MAX_NAME];
    uint32_t start_lba;
    uint32_t size;
    uint32_t used;
    uint32_t reserved;
} dir_entry_t;

typedef struct {
    char magic[4]; // "RTFS"
    uint32_t version;
    uint32_t total_sectors;
    uint32_t dir_start;
    uint32_t dir_count;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t data_start;
    uint32_t sector_size;
} superblock_t;

extern int ata_read_sector(uint32_t lba, uint8_t* buf);
extern int ata_write_sector(uint32_t lba, const uint8_t* buf);
extern void* kmalloc(uint32_t size);
extern int kfree(void* ptr);
extern void print(const char* s);
extern void print_char(char c);

static int fs_ok = 0;
static superblock_t sb;
static dir_entry_t* dir = 0;
static uint8_t* bitmap = 0;
static uint32_t dir_sectors = 0;
static uint32_t data_count = 0; // data sectors on disk
static uint8_t* sector_buf = 0;

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void my_strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
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

static int bitmap_get(uint32_t i) {
    return (bitmap[i / 8] >> (i % 8)) & 1;
}

static void bitmap_set(uint32_t i, int v) {
    if (v) bitmap[i / 8] |= (1 << (i % 8));
    else bitmap[i / 8] &= ~(1 << (i % 8));
}

static int write_back_dir(void) {
    for (uint32_t s = 0; s < dir_sectors; s++) {
        uint8_t* src = (uint8_t*)dir + s * SECTOR_SIZE;
        for (int i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = src[i];
        if (ata_write_sector(sb.dir_start + s, sector_buf) != 0) return -1;
    }
    return 0;
}

static int write_back_bitmap(void) {
    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        uint8_t* src = bitmap + s * SECTOR_SIZE;
        for (int i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = src[i];
        if (ata_write_sector(sb.bitmap_start + s, sector_buf) != 0) return -1;
    }
    return 0;
}

static int find_entry(const char* name) {
    for (uint32_t i = 0; i < sb.dir_count; i++) {
        if (dir[i].used && my_strcmp(dir[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int free_slot(void) {
    for (uint32_t i = 0; i < sb.dir_count; i++) {
        if (!dir[i].used) return (int)i;
    }
    return -1;
}

// First run of n consecutive free data sectors, as a data-relative
// index, or -1 if none fits. Contiguity is a filesystem rule here,
// not a disk rule: the disk does not care where a file's sectors
// are, but this filesystem promises one run per file, so a file
// that cannot fit in one run fails instead of fragmenting.
static int alloc_contiguous(uint32_t n) {
    if (n == 0) return 0;
    for (uint32_t start = 0; start + n <= data_count; start++) {
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            if (bitmap_get(start + i)) { ok = 0; break; }
        }
        if (ok) return (int)start;
    }
    return -1;
}

void diskfs_init(void) {
    sector_buf = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (!sector_buf) { print("Disk: out of memory, files unavailable\n"); return; }
    if (ata_read_sector(0, sector_buf) != 0) {
        print("Disk: not found, files unavailable\n");
        return;
    }
    superblock_t* s = (superblock_t*)sector_buf;
    if (s->magic[0] != 'R' || s->magic[1] != 'T' || s->magic[2] != 'F' || s->magic[3] != 'S' ||
        s->sector_size != SECTOR_SIZE || s->dir_count == 0 || s->bitmap_sectors == 0) {
        print("Disk: no RingTail filesystem, files unavailable\n");
        return;
    }
    sb = *s;
    dir_sectors = (sb.dir_count * DIR_ENTRY_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
    data_count = sb.total_sectors > sb.data_start ? sb.total_sectors - sb.data_start : 0;
    dir = (dir_entry_t*)kmalloc(dir_sectors * SECTOR_SIZE);
    bitmap = (uint8_t*)kmalloc(sb.bitmap_sectors * SECTOR_SIZE);
    if (!dir || !bitmap) {
        print("Disk: out of memory, files unavailable\n");
        return;
    }
    for (uint32_t i = 0; i < dir_sectors * SECTOR_SIZE; i++) ((uint8_t*)dir)[i] = 0;
    for (uint32_t i = 0; i < sb.bitmap_sectors * SECTOR_SIZE; i++) bitmap[i] = 0;
    for (uint32_t sec = 0; sec < dir_sectors; sec++) {
        if (ata_read_sector(sb.dir_start + sec, sector_buf) != 0) {
            print("Disk: read error, files unavailable\n");
            return;
        }
        for (int i = 0; i < SECTOR_SIZE; i++) ((uint8_t*)dir)[sec * SECTOR_SIZE + i] = sector_buf[i];
    }
    for (uint32_t sec = 0; sec < sb.bitmap_sectors; sec++) {
        if (ata_read_sector(sb.bitmap_start + sec, sector_buf) != 0) {
            print("Disk: read error, files unavailable\n");
            return;
        }
        for (int i = 0; i < SECTOR_SIZE; i++) bitmap[sec * SECTOR_SIZE + i] = sector_buf[i];
    }
    fs_ok = 1;
    int count = 0;
    for (uint32_t i = 0; i < sb.dir_count; i++) if (dir[i].used) count++;
    print("Disk: RTFS, ");
    print_num(count);
    print(" files\n");
}

int diskfs_create(const char* name) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = 0;
    if (find_entry(name) >= 0) result = -1;
    else if (my_strlen(name) >= MAX_NAME) result = -2;
    else {
        int slot = free_slot();
        if (slot < 0) result = -3;
        else {
            dir[slot].used = 1;
            dir[slot].start_lba = 0;
            dir[slot].size = 0;
            dir[slot].reserved = 0;
            my_strcpy(dir[slot].name, name);
            if (write_back_dir() != 0) {
                dir[slot].used = 0;
                result = -3;
            }
        }
    }
    irq_restore(flags);
    return result;
}

// Length-based write, the real implementation. The string
// version below is a thin wrapper for the shell, which passes
// text. Programs pass bytes through syscalls, and bytes are not
// strings: a program file may contain null bytes, and strlen
// would truncate it at the first one, so the syscall path must
// carry its length explicitly. This function takes the length
// as given and writes exactly that many bytes, nulls included.
int diskfs_write_bytes(const char* name, const uint8_t* content, uint32_t len) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = 0;
    int idx = find_entry(name);
    if (idx < 0) {
        // Auto-create, like ramfs: writing a new file creates it.
        if (my_strlen(name) >= MAX_NAME) result = -2;
        else {
            int slot = free_slot();
            if (slot < 0) result = -3;
            else {
                dir[slot].used = 1;
                dir[slot].start_lba = 0;
                dir[slot].size = 0;
                dir[slot].reserved = 0;
                my_strcpy(dir[slot].name, name);
                idx = slot;
            }
        }
    }
    if (result == 0) {
        uint32_t nsec = (len + SECTOR_SIZE - 1) / SECTOR_SIZE;
        int alloc = -1;
        if (nsec > 0) {
            alloc = alloc_contiguous(nsec);
            if (alloc < 0) result = -2; // no contiguous run fits
        }
        if (result == 0) {
            // Mark the new run first, so a later allocation in
            // this same call cannot reuse it, then write the data
            // sectors, then free the old run, then point the
            // entry at the new run. Allocate-before-free, as in
            // ramfs: a failed write leaves the old file intact.
            for (uint32_t i = 0; i < nsec; i++) bitmap_set((uint32_t)alloc + i, 1);
            int werr = 0;
            for (uint32_t sec = 0; sec < nsec && !werr; sec++) {
                for (int i = 0; i < SECTOR_SIZE; i++) {
                    uint32_t off = sec * SECTOR_SIZE + (uint32_t)i;
                    sector_buf[i] = off < len ? (uint8_t)content[off] : 0;
                }
                if (ata_write_sector(sb.data_start + (uint32_t)alloc + sec, sector_buf) != 0) werr = 1;
            }
            if (werr) {
                // Undo the allocation in RAM; nothing was written
                // back yet, so disk still matches RAM again.
                for (uint32_t i = 0; i < nsec; i++) bitmap_set((uint32_t)alloc + i, 0);
                if (dir[idx].size == 0 && dir[idx].start_lba == 0) {
                    // This was a fresh auto-created entry: remove it.
                    dir[idx].used = 0;
                }
                result = -2;
            } else {
                uint32_t old_start = dir[idx].start_lba;
                uint32_t old_nsec = (dir[idx].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
                if (old_nsec > 0 && old_start >= sb.data_start) {
                    uint32_t old_rel = old_start - sb.data_start;
                    for (uint32_t i = 0; i < old_nsec; i++) bitmap_set(old_rel + i, 0);
                }
                dir[idx].start_lba = nsec > 0 ? sb.data_start + (uint32_t)alloc : 0;
                dir[idx].size = len;
                if (write_back_bitmap() != 0 || write_back_dir() != 0) result = -2;
            }
        }
    }
    irq_restore(flags);
    return result;
}

// String write, for the shell: the text's length is its string
// length, and the bytes version does the work.
int diskfs_write(const char* name, const char* content) {
    return diskfs_write_bytes(name, (const uint8_t*)content, (uint32_t)my_strlen(content));
}

int diskfs_read(const char* name, char* out, uint32_t max) {
    if (!fs_ok) return -4;
    if (max == 0) return -1;
    uint32_t flags = irq_save();
    int result = -1;
    int idx = find_entry(name);
    if (idx < 0) result = -1;
    else if (dir[idx].size == 0) {
        out[0] = 0;
        result = 0;
    } else {
        uint32_t to_copy = dir[idx].size;
        if (to_copy >= max) to_copy = max - 1;
        uint32_t copied = 0;
        int rerr = 0;
        while (copied < to_copy && !rerr) {
            uint32_t sec = copied / SECTOR_SIZE;
            if (ata_read_sector(dir[idx].start_lba + sec, sector_buf) != 0) { rerr = 1; break; }
            uint32_t off = copied % SECTOR_SIZE;
            while (off < SECTOR_SIZE && copied < to_copy) {
                out[copied++] = (char)sector_buf[off++];
            }
        }
        if (rerr) result = -1;
        else {
            out[copied] = 0;
            result = (int)copied;
        }
    }
    irq_restore(flags);
    return result;
}

int diskfs_delete(const char* name) {
    if (!fs_ok) return -4;
    uint32_t flags = irq_save();
    int result = -1;
    int idx = find_entry(name);
    if (idx >= 0) {
        uint32_t nsec = (dir[idx].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (nsec > 0 && dir[idx].start_lba >= sb.data_start) {
            uint32_t rel = dir[idx].start_lba - sb.data_start;
            for (uint32_t i = 0; i < nsec; i++) bitmap_set(rel + i, 0);
        }
        dir[idx].used = 0;
        dir[idx].start_lba = 0;
        dir[idx].size = 0;
        dir[idx].name[0] = 0;
        if (write_back_bitmap() != 0 || write_back_dir() != 0) result = -1;
        else result = 0;
    }
    irq_restore(flags);
    return result;
}

void diskfs_list(void) {
    if (!fs_ok) { print("no disk\n"); return; }
    uint32_t flags = irq_save();
    int any = 0;
    for (uint32_t i = 0; i < sb.dir_count; i++) {
        if (dir[i].used) {
            any = 1;
            print(dir[i].name);
            print(" (");
            print_num(dir[i].size);
            print(" bytes)\n");
        }
    }
    if (!any) print("empty\n");
    irq_restore(flags);
}

int diskfs_exists(const char* name) {
    if (!fs_ok) return 0;
    uint32_t flags = irq_save();
    int result = find_entry(name) >= 0;
    irq_restore(flags);
    return result;
}

// Loader support: read a whole file into a heap buffer. Returns
// the buffer, or 0 on failure, with the file size in *size_out.
// The caller frees the buffer with kfree. Size is capped at
// 64 KB: programs in this slice are tiny, and an unbounded read
// would let a corrupt directory entry eat the heap.
uint8_t* diskfs_read_all(const char* name, uint32_t* size_out) {
    if (!fs_ok) return 0;
    uint32_t flags = irq_save();
    uint8_t* buf = 0;
    int idx = find_entry(name);
    if (idx < 0 || dir[idx].size > 65536) { irq_restore(flags); return 0; }
    uint32_t size = dir[idx].size;
    // Allocate outside the lock? kmalloc is itself locked, and
    // save/restore nests, so allocating here is safe.
    buf = (uint8_t*)kmalloc(size > 0 ? size : 1);
    if (!buf) { irq_restore(flags); return 0; }
    uint32_t copied = 0;
    int rerr = 0;
    while (copied < size && !rerr) {
        uint32_t sec = copied / SECTOR_SIZE;
        if (ata_read_sector(dir[idx].start_lba + sec, sector_buf) != 0) { rerr = 1; break; }
        uint32_t off = copied % SECTOR_SIZE;
        while (off < SECTOR_SIZE && copied < size) buf[copied++] = sector_buf[off++];
    }
    if (rerr) {
        kfree(buf);
        buf = 0;
    } else {
        *size_out = size;
    }
    irq_restore(flags);
    return buf;
}

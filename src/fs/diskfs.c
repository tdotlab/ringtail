// FAT16 filesystem on the primary ATA disk, with subdirectories.
//
// Layout (all derived from the BPB in sector 0, so images made by
// other tools work): reserved sectors, the FAT copies, the fixed
// root directory, then the data area where cluster 2 is the first
// data cluster. A subdirectory is a cluster chain of 32-byte entries like the root, starting with
// "." and "..", and grows a cluster at a time; the root cannot grow.
//
// The FAT and the root directory are held in memory, and
// subdirectory sectors go through a small cache. Mutations mark what
// they touch dirty, and commit() writes it back (every FAT copy, then
// the directories) and flushes the disk cache. File data and new
// directory clusters are written before the metadata that points at
// them, so a crash can leak clusters but not expose another file's
// data.
//
// A file or directory in use is a node. A file is known by where its
// directory entry sits, a directory by its first cluster; the root
// is node 0, cluster 0. Nothing with a node can be deleted, which is
// what keeps open files and current directories safe.
//
// Names are long (VFAT): a name that is not 8.3 in one case gets
// long-name slots before its short entry, and a short alias such as
// MYNOTE~1.TXT, as Windows makes them. A name that fits 8.3 gets only
// the short entry, with the NT case bits for lowercase. Case is kept
// and names match in any case, by their long or short name.
//
// Not supported: timestamps, FAT12 and FAT32, and characters past
// ASCII in long names (such a file is reached by its short name).
//
// Concurrency: one mutex around every public call. Interrupts stay
// on, so the disk wait does not freeze the machine.
#include "fs.h"
#include "lib.h"
#include "mm.h"
#include "drivers.h"
#include "ringtail/abi.h"

#ifdef RINGTAIL_KERNEL
#include "sync.h"
static mutex_t fs_lock = MUTEX_INIT;
#define LOCK()   mutex_lock(&fs_lock)
#define UNLOCK() mutex_unlock(&fs_lock)
#else
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

#define SECTOR 512u
#define FAT_EOC 0xFFFFu
#define FAT_EOC_MIN 0xFFF8u
#define ATTR_VOLUME 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0F
#define DIR_ATTR 11
#define DIR_FIRST_HIGH 20
#define DIR_FIRST_LOW 26
#define DIR_SIZE 28
#define MAX_FATS 4
#define PER_SECTOR 16       // directory entries in a sector
#define MAX_DIR_ENTRIES 65536u  // FAT's limit; bounds a walk on a looped chain
#define DCACHE 16           // cached subdirectory sectors
#define MAX_NODES 64

static int fs_ok;
static uint32_t spc;            // sectors per cluster
static uint32_t cbytes;         // bytes per cluster
static uint32_t num_fats, fat_sectors, fat_start;
static uint32_t root_entries, root_start, root_sectors;
static uint32_t data_start, cluster_count;
static uint32_t free_hint = 2;

static uint16_t* fat;           // fat_sectors * 256 entries
static uint8_t* root;           // root_sectors * 512 bytes
static uint8_t* fat_dirty;      // one flag per FAT sector
static uint8_t* root_dirty;     // one flag per root sector
static uint8_t* secbuf;         // scratch sector, used under the lock

static const uint8_t DOT[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
static const uint8_t DOTDOT[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// ---- Names ----

#define NT_LOWER_BASE 0x08  // byte 12 of a short entry: the base is shown in lowercase
#define NT_LOWER_EXT  0x10  // and the extension
#define LFN_LAST 0x40       // on the first long-name slot, which holds the last piece
#define LFN_MAX_SLOTS 20    // 20 * 13 characters cover 255

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }

// Long names are printable ASCII without the characters FAT forbids.
// Names other systems wrote with anything else are reached by their
// short names.
static int long_char_ok(char c) {
    return (unsigned char)c >= 0x20 && (unsigned char)c < 0x7F && !strchr("\"*/:<>?\\|", c);
}

// A short name also has no spaces, dots (but the one) or "+,;=[]".
static int short_char_ok(char c) {
    return (unsigned char)c > 0x20 && (unsigned char)c < 0x7F && !strchr("\"*+,./:;<=>?[\\]|", c);
}

// 1 to NAME_MAX of those characters, not starting with a space and
// not ending in a space or a dot: Windows drops those, so the name
// would not survive the trip. That rules out "." and ".." too.
static int long_name_ok(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n > NAME_MAX) return 0;
    if (name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++)
        if (!long_char_ok(name[i])) return 0;
    return 1;
}

int diskfs_name_ok(const char* name) { return name && long_name_ok(name); }

// 1 if name is an 8.3 name as it stands, each part in a single case:
// then a short entry holds it, with case bits for lowercase parts.
// Its raw form goes in raw and the case bits in *nt. 0 if it needs a
// long name.
static int fits_short(const char* name, uint8_t raw[11], uint8_t* nt) {
    const char* dot = strchr(name, '.');
    size_t len = strlen(name);
    size_t base = dot ? (size_t)(dot - name) : len;
    size_t ext = dot ? len - base - 1 : 0;
    if (base < 1 || base > 8 || ext > 3 || (dot && ext == 0)) return 0;
    memset(raw, ' ', 11);
    int up[2] = { 0, 0 }, low[2] = { 0, 0 };
    for (size_t i = 0; i < len; i++) {
        if (i == base) continue;  // the dot
        char c = name[i];
        if (!short_char_ok(c)) return 0;
        int part = i > base;
        up[part] |= is_upper(c);
        low[part] |= is_lower(c);
        raw[part ? 8 + (i - base - 1) : i] = (uint8_t)upper(c);
    }
    if ((up[0] && low[0]) || (up[1] && low[1])) return 0;
    *nt = (uint8_t)((low[0] ? NT_LOWER_BASE : 0) | (low[1] ? NT_LOWER_EXT : 0));
    return 1;
}

// A short entry's name as shown: trailing spaces dropped, its case
// bits applied.
static void short_display(const uint8_t* e, char out[13]) {
    int b = 8, x = 3, o = 0;
    while (b > 0 && e[b - 1] == ' ') b--;
    while (x > 0 && e[8 + x - 1] == ' ') x--;
    for (int i = 0; i < b; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);  // 0x05 stands for a leading 0xE5
        out[o++] = (e[12] & NT_LOWER_BASE) && is_upper(c) ? (char)(c + 32) : c;
    }
    if (x > 0) {
        out[o++] = '.';
        for (int i = 0; i < x; i++) {
            char c = (char)e[8 + i];
            out[o++] = (e[12] & NT_LOWER_EXT) && is_upper(c) ? (char)(c + 32) : c;
        }
    }
    out[o] = 0;
}

// The start of a short name for a long one, Windows' way: uppercase,
// spaces and all but the last dot dropped, characters a short name
// cannot hold made '_'. *lossy says whether more than case was lost,
// in which case it gets a "~N" tail.
static void short_basis(const char* name, uint8_t raw[11], int* baselen, int* lossy) {
    memset(raw, ' ', 11);
    *lossy = 0;
    const char* s = name;
    while (*s == '.') { s++; *lossy = 1; }
    const char* dot = strrchr(s, '.');
    int b = 0, x = 0;
    for (const char* p = s; *p && p != dot; p++) {
        char c = *p;
        if (c == ' ' || c == '.') { *lossy = 1; continue; }
        if (!short_char_ok(c)) { c = '_'; *lossy = 1; }
        if (b < 8) raw[b++] = (uint8_t)upper(c);
        else *lossy = 1;
    }
    for (const char* p = dot ? dot + 1 : ""; *p; p++) {
        char c = *p;
        if (c == ' ') { *lossy = 1; continue; }
        if (!short_char_ok(c)) { c = '_'; *lossy = 1; }
        if (x < 3) raw[8 + x++] = (uint8_t)upper(c);
        else *lossy = 1;
    }
    if (b == 0) { raw[0] = '_'; b = 1; *lossy = 1; }
    *baselen = b;
}

// The checksum long-name slots carry of their short entry's name.
static uint8_t lfn_sum(const uint8_t raw[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + raw[i]);
    return s;
}

// Where a long-name slot keeps its 13 UCS-2 characters.
static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// ---- FAT ----

static int valid_cluster(uint32_t c) { return c >= 2 && c < cluster_count + 2; }
static uint32_t fat_get(uint32_t c) { return fat[c]; }
static void fat_set(uint32_t c, uint32_t v) {
    fat[c] = (uint16_t)v;
    fat_dirty[(c * 2) / SECTOR] = 1;
}
static uint32_t cluster_lba(uint32_t c) { return data_start + (c - 2) * spc; }

static uint32_t alloc_cluster(void) {
    for (uint32_t n = 0; n < cluster_count; n++) {
        uint32_t c = 2 + (free_hint - 2 + n) % cluster_count;
        if (fat_get(c) == 0) {
            fat_set(c, FAT_EOC);
            free_hint = c + 1 < cluster_count + 2 ? c + 1 : 2;
            return c;
        }
    }
    return 0;
}

static void dcache_forget(uint32_t lba, uint32_t count);

// Bounded, so a looped chain on a corrupt disk cannot hang us. A
// freed directory's sectors leave the cache, so the cluster can come
// back as anything.
static void free_chain(uint32_t c) {
    for (uint32_t steps = 0; valid_cluster(c) && steps < cluster_count; steps++) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        dcache_forget(cluster_lba(c), spc);
        if (c < free_hint) free_hint = c;
        if (next >= FAT_EOC_MIN) break;
        c = next;
    }
}

// ---- Directory sectors ----

// Subdirectory sectors, read on demand. A slot with unwritten
// changes stays until commit(); a public call dirties a few at most.
static uint8_t dcache_data[DCACHE][SECTOR];
static struct { uint32_t lba, stamp; uint8_t valid, dirty; } dcache[DCACHE];
static uint32_t dcache_clock;

static void dcache_forget(uint32_t lba, uint32_t count) {
    for (int i = 0; i < DCACHE; i++)
        if (dcache[i].valid && dcache[i].lba >= lba && dcache[i].lba < lba + count)
            dcache[i].valid = dcache[i].dirty = 0;
}

// The directory sector at lba, in memory, marked dirty if asked.
// NULL on a read error.
static uint8_t* dir_sector(uint32_t lba, int dirty) {
    if (lba >= root_start && lba < root_start + root_sectors) {
        if (dirty) root_dirty[lba - root_start] = 1;
        return root + (lba - root_start) * SECTOR;
    }
    int slot = -1;
    for (int i = 0; i < DCACHE && slot < 0; i++)
        if (dcache[i].valid && dcache[i].lba == lba) slot = i;
    if (slot < 0) {
        for (int i = 0; i < DCACHE; i++) {
            if (dcache[i].dirty) continue;
            if (!dcache[i].valid) { slot = i; break; }
            if (slot < 0 || dcache[i].stamp < dcache[slot].stamp) slot = i;
        }
        if (slot < 0) return 0;
        dcache[slot].valid = 0;
        if (ata_read(lba, 1, dcache_data[slot]) != 0) return 0;
        dcache[slot].lba = lba;
        dcache[slot].valid = 1;
    }
    dcache[slot].stamp = ++dcache_clock;
    if (dirty) dcache[slot].dirty = 1;
    return dcache_data[slot];
}

static int commit(void) {
    int err = 0;
    for (uint32_t s = 0; s < fat_sectors; s++) {
        if (!fat_dirty[s]) continue;
        for (uint32_t f = 0; f < num_fats; f++)
            if (ata_write(fat_start + f * fat_sectors + s, 1, (uint8_t*)fat + s * SECTOR) != 0) err = -EIO;
        fat_dirty[s] = 0;
    }
    for (uint32_t s = 0; s < root_sectors; s++) {
        if (!root_dirty[s]) continue;
        if (ata_write(root_start + s, 1, root + s * SECTOR) != 0) err = -EIO;
        root_dirty[s] = 0;
    }
    for (int i = 0; i < DCACHE; i++) {
        if (!dcache[i].dirty) continue;
        if (ata_write(dcache[i].lba, 1, dcache_data[i]) != 0) err = -EIO;
        dcache[i].dirty = 0;
    }
    if (ata_flush() != 0) err = -EIO;
    return err;
}

// ---- Directory entries ----

// Where an entry sits: its sector's LBA times 16, plus its slot there.
typedef uint32_t loc_t;
#define LOC(lba, slot) ((lba) * PER_SECTOR + (slot))

// Entries are worked on as 32-byte copies and put back.
static int ent_get(loc_t loc, uint8_t e[32]) {
    uint8_t* s = dir_sector(loc / PER_SECTOR, 0);
    if (!s) return -EIO;
    memcpy(e, s + (loc % PER_SECTOR) * 32, 32);
    return 0;
}

static int ent_put(loc_t loc, const uint8_t e[32]) {
    uint8_t* s = dir_sector(loc / PER_SECTOR, 1);
    if (!s) return -EIO;
    memcpy(s + (loc % PER_SECTOR) * 32, e, 32);
    return 0;
}

static uint32_t entry_first(const uint8_t* e) { return rd16(e + DIR_FIRST_LOW); }
static uint32_t entry_size(const uint8_t* e) { return rd32(e + DIR_SIZE); }
static void set_first(uint8_t* e, uint32_t c) { wr16(e + DIR_FIRST_LOW, (uint16_t)c); }
static void set_size(uint8_t* e, uint32_t s) { wr32(e + DIR_SIZE, s); }

static void make_entry(uint8_t* e, const uint8_t raw[11], uint8_t attr, uint32_t first) {
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[DIR_ATTR] = attr;
    set_first(e, first);
}

static int ent_end(const uint8_t* e) { return e[0] == 0x00; }  // nothing after it is used
static int ent_free(const uint8_t* e) { return e[0] == 0x00 || e[0] == 0xE5; }
static int ent_is_dir(const uint8_t* e) { return (e[DIR_ATTR] & ATTR_DIRECTORY) != 0; }
static int ent_is_dot(const uint8_t* e) { return e[0] == '.'; }  // "." or ".."; no 8.3 name starts so

// A file or directory of ours: not free, not a piece of a long name,
// not the volume label, not a FAT32 leftover.
static int ent_usable(const uint8_t* e) {
    if (ent_free(e) || e[DIR_ATTR] == ATTR_LFN || (e[DIR_ATTR] & ATTR_VOLUME)) return 0;
    return rd16(e + DIR_FIRST_HIGH) == 0;
}

// Walks the slots of a directory: the root's fixed table, or a
// subdirectory's cluster chain. n counts the slots handed out.
typedef struct { uint32_t dcl, cl, i, n; } diter_t;

static void dir_begin(diter_t* it, uint32_t dcl) {
    it->dcl = it->cl = dcl;
    it->i = it->n = 0;
}

// 1 with the next slot in *loc, 0 past the last one, -EIO on a
// broken chain.
static int dir_next(diter_t* it, loc_t* loc) {
    if (it->dcl && it->n == 0 && !valid_cluster(it->dcl)) return -EIO;
    uint32_t per = it->dcl ? cbytes / 32 : root_entries;
    if (it->i == per) {
        if (!it->dcl) return 0;
        uint32_t next = fat_get(it->cl);
        if (next >= FAT_EOC_MIN) return 0;
        if (!valid_cluster(next) || it->n >= MAX_DIR_ENTRIES) return -EIO;
        it->cl = next;
        it->i = 0;
    }
    uint32_t lba = (it->dcl ? cluster_lba(it->cl) : root_start) + it->i / PER_SECTOR;
    *loc = LOC(lba, it->i % PER_SECTOR);
    it->i++;
    it->n++;
    return 1;
}

// An entry as a directory listing sees it: the short entry, with the
// long name put together from the slots before it.
typedef struct {
    diter_t start;            // just before the entry's first slot
    uint32_t slots;           // long-name slots plus the short one
    loc_t loc;                // the short entry, which is what a file node keys on
    uint8_t e[32];            // a copy of it
    char name[NAME_MAX + 1];  // the long name, or the short one when there is none
    char alias[13];           // the short name
} dent_t;

// The next entry of a directory: 1 with d filled, 0 past the last,
// -EIO. Long-name slots count only when they run unbroken up to the
// short entry and carry its checksum; otherwise the short name
// stands. With all, the volume label and FAT32 leftovers come too
// (for refusing a name that is taken); callers check ent_usable().
static int dir_read(diter_t* it, dent_t* d, int all) {
    char lfn[LFN_MAX_SLOTS * 13 + 1];
    int seq = 0, total = 0, bad = 0;  // seq: the piece last seen, 0 when no long name is pending
    uint8_t sum = 0;
    uint8_t* e = d->e;
    for (;;) {
        diter_t before = *it;
        loc_t l;
        int r = dir_next(it, &l);
        if (r <= 0) return r;
        if (ent_get(l, e)) return -EIO;
        if (ent_end(e)) return 0;
        if (ent_free(e)) { seq = 0; continue; }
        if (e[DIR_ATTR] == ATTR_LFN) {
            int ord = e[0] & 0x1F;
            if (e[0] & LFN_LAST) {
                if (ord < 1 || ord > LFN_MAX_SLOTS) { seq = 0; continue; }
                d->start = before;
                total = ord;
                sum = e[13];
                bad = 0;
            } else if (!seq || ord != seq - 1 || e[13] != sum) {
                seq = 0;
                continue;
            }
            seq = ord;
            for (int i = 0; i < 13; i++) {
                uint16_t ch = rd16(e + lfn_off[i]);
                char c = 0;
                if (ch != 0 && ch != 0xFFFF) {
                    if (ch < 0x20 || ch >= 0x7F) bad = 1;
                    c = (char)ch;
                }
                lfn[(ord - 1) * 13 + i] = c;
            }
            continue;
        }
        if (!all && !ent_usable(e)) { seq = 0; continue; }
        d->loc = l;
        short_display(e, d->alias);
        int have = seq == 1 && !bad && sum == lfn_sum(e);
        if (have) {
            lfn[total * 13] = 0;
            size_t n = strlen(lfn);
            have = n > 0 && n <= NAME_MAX;
            if (have) memcpy(d->name, lfn, n + 1);
        }
        if (have) {
            d->slots = (uint32_t)total + 1;
        } else {
            d->start = before;
            d->slots = 1;
            memcpy(d->name, d->alias, sizeof d->alias);
        }
        return 1;
    }
}

// Look for name in directory dcl, by its long name or its short one,
// in any case: 1 with d filled, 0 if it is not there, -EIO.
static int dir_find(uint32_t dcl, const char* name, int all, dent_t* d) {
    diter_t it;
    dir_begin(&it, dcl);
    int r;
    while ((r = dir_read(&it, d, all)) > 0)
        if (strcasecmp(d->name, name) == 0 || strcasecmp(d->alias, name) == 0) return 1;
    return r;
}

// The name of directory c as its parent lists it.
static int dir_name(uint32_t parent, uint32_t c, char name[NAME_MAX + 1]) {
    diter_t it;
    dir_begin(&it, parent);
    dent_t d;
    int r;
    while ((r = dir_read(&it, &d, 0)) > 0) {
        if (ent_is_dir(d.e) && !ent_is_dot(d.e) && entry_first(d.e) == c) {
            memcpy(name, d.name, strlen(d.name) + 1);
            return 0;
        }
    }
    return r < 0 ? r : -EIO;
}

// 1 if directory dcl holds nothing but "." and "..", 0 if it holds
// more, -EIO.
static int dir_empty(uint32_t dcl) {
    diter_t it;
    dir_begin(&it, dcl);
    dent_t d;
    int r;
    while ((r = dir_read(&it, &d, 0)) > 0)
        if (!ent_is_dot(d.e)) return 0;
    return r < 0 ? r : 1;
}

// A fresh directory cluster, written straight to the disk before
// anything points at it: zeros, plus "." and ".." for a new directory.
static int init_dir_cluster(uint32_t c, int dots, uint32_t parent) {
    for (uint32_t s = 0; s < spc; s++) {
        memset(secbuf, 0, SECTOR);
        if (s == 0 && dots) {
            make_entry(secbuf, DOT, ATTR_DIRECTORY, c);
            make_entry(secbuf + 32, DOTDOT, ATTR_DIRECTORY, parent);
        }
        if (ata_write(cluster_lba(c) + s, 1, secbuf) != 0) return -EIO;
    }
    dcache_forget(cluster_lba(c), spc);
    return 0;
}

// n free slots in a row in directory dcl, with *start just before the
// first. A subdirectory grows by as many clusters as that takes; a
// full root is -ENOSPC.
static int dir_slots(uint32_t dcl, uint32_t n, diter_t* start) {
    diter_t it, before;
    dir_begin(&it, dcl);
    uint32_t run = 0;
    loc_t l;
    uint8_t e[32];
    int r;
    for (;;) {
        before = it;
        r = dir_next(&it, &l);
        if (r <= 0) break;
        if (ent_get(l, e)) return -EIO;
        if (!ent_free(e)) { run = 0; continue; }
        if (run++ == 0) *start = before;
        if (run == n) return 0;
    }
    if (r < 0) return r;
    if (!dcl || it.n + (n - run) > MAX_DIR_ENTRIES) return -ENOSPC;
    if (run == 0) *start = it;  // the run starts in the first new cluster
    uint32_t per = cbytes / 32;
    uint32_t last = it.cl;
    for (uint32_t k = 0; k < (n - run + per - 1) / per; k++) {
        uint32_t c = alloc_cluster();
        if (!c) return -ENOSPC;
        if (init_dir_cluster(c, 0, 0)) { fat_set(c, 0); return -EIO; }
        fat_set(last, c);
        last = c;
    }
    return 0;
}

// Used "~N" tails, one bit each; a directory cannot hold more names
// than this, so one is always free.
static uint8_t tail_used[(MAX_DIR_ENTRIES + 2) / 8 + 1];

// Give the short basis in raw (baselen characters before the
// extension) the lowest "~N" tail no short name in dcl has.
static int numeric_tail(uint32_t dcl, uint8_t raw[11], int baselen) {
    memset(tail_used, 0, sizeof tail_used);
    diter_t it;
    dir_begin(&it, dcl);
    loc_t l;
    uint8_t e[32];
    int r;
    while ((r = dir_next(&it, &l)) > 0) {
        if (ent_get(l, e)) return -EIO;
        if (ent_end(e)) break;
        if (ent_free(e) || e[DIR_ATTR] == ATTR_LFN || memcmp(e + 8, raw + 8, 3)) continue;
        int t = 0;
        while (t < 8 && e[t] != '~') t++;
        int k = t + 1;
        uint32_t num = 0;
        while (k < 8 && e[k] >= '0' && e[k] <= '9' && num <= MAX_DIR_ENTRIES + 1) num = num * 10 + (uint32_t)(e[k++] - '0');
        int digits = k - t - 1;
        while (k < 8 && e[k] == ' ') k++;
        if (t >= 8 || digits == 0 || k != 8 || num == 0 || num > MAX_DIR_ENTRIES + 1) continue;
        if (t != MIN(baselen, 7 - digits) || memcmp(e, raw, (size_t)t)) continue;
        tail_used[num / 8] |= (uint8_t)(1 << (num % 8));
    }
    if (r < 0) return r;
    for (uint32_t num = 1; num <= MAX_DIR_ENTRIES + 1; num++) {
        if (tail_used[num / 8] & (1 << (num % 8))) continue;
        char tail[12];
        int digits = snprintf(tail, sizeof tail, "~%u", num) - 1;
        int t = MIN(baselen, 7 - digits);
        memset(raw + t, ' ', (size_t)(8 - t));
        memcpy(raw + t, tail, (size_t)digits + 1);
        return 0;
    }
    return -ENOSPC;
}

// Add an entry for name to directory dcl, made from tmpl (attribute,
// cluster, size; the name and its case bits are set here): the
// long-name slots it needs, then the short entry, whose place goes in
// *loc. The caller has made sure the name is free. With keep (an
// entry being renamed to its own name in another case, so the same
// basis), its short name is kept rather than given a new tail.
static int dir_add(uint32_t dcl, const char* name, const uint8_t tmpl[32], const uint8_t* keep, loc_t* loc) {
    uint8_t e[32], raw[11], nt = 0;
    uint32_t len = (uint32_t)strlen(name), longs = 0;
    if (!fits_short(name, raw, &nt)) {
        int baselen, lossy;
        short_basis(name, raw, &baselen, &lossy);
        // Only case was lost ("ReadMe.txt"): the plain short name is
        // free, or name would have matched it.
        if (lossy && keep) {
            memcpy(raw, keep, 11);
        } else if (lossy) {
            int r = numeric_tail(dcl, raw, baselen);
            if (r) return r;
        }
        longs = (len + 12) / 13;
    }
    memcpy(e, tmpl, 32);
    memcpy(e, raw, 11);
    e[12] = (uint8_t)((e[12] & ~(NT_LOWER_BASE | NT_LOWER_EXT)) | nt);
    diter_t it;
    int r = dir_slots(dcl, longs + 1, &it);
    if (r) return r;
    uint8_t sum = lfn_sum(raw);
    for (uint32_t k = longs;; k--) {  // the last piece of the name comes first
        loc_t l;
        if (dir_next(&it, &l) <= 0) return -EIO;
        if (k == 0) {
            *loc = l;
            return ent_put(l, e);
        }
        uint8_t x[32];
        memset(x, 0, sizeof x);
        x[0] = (uint8_t)(k | (k == longs ? LFN_LAST : 0));
        x[DIR_ATTR] = ATTR_LFN;
        x[13] = sum;
        for (int i = 0; i < 13; i++) {
            uint32_t pos = (k - 1) * 13 + (uint32_t)i;
            wr16(x + lfn_off[i], pos < len ? (uint8_t)name[pos] : pos == len ? 0 : 0xFFFF);
        }
        r = ent_put(l, x);
        if (r) return r;
    }
}

// Mark every slot of an entry free.
static int dir_remove(const dent_t* d) {
    diter_t it = d->start;
    for (uint32_t k = 0; k < d->slots; k++) {
        loc_t l;
        uint8_t e[32];
        if (dir_next(&it, &l) <= 0 || ent_get(l, e)) return -EIO;
        e[0] = 0xE5;
        int r = ent_put(l, e);
        if (r) return r;
    }
    return 0;
}

// Directory dcl's parent, from its ".." entry. The root is its own.
static int dir_parent(uint32_t dcl, uint32_t* parent) {
    if (dcl == 0) { *parent = 0; return 0; }
    dent_t d;
    if (dir_find(dcl, "..", 0, &d) <= 0 || !ent_is_dir(d.e)) return -EIO;
    uint32_t p = entry_first(d.e);
    if (p && !valid_cluster(p)) return -EIO;
    *parent = p;
    return 0;
}

// The absolute path of directory dcl, climbing ".." to the root and
// looking up each directory's name in its parent. Returns the length.
static int dir_path(uint32_t dcl, char* buf, uint32_t size) {
    char tmp[PATH_MAX];
    uint32_t pos = sizeof tmp - 1;
    tmp[pos] = 0;
    while (dcl) {  // ends at the root, or when the buffer fills on a looped disk
        uint32_t parent;
        char name[NAME_MAX + 1];
        int r = dir_parent(dcl, &parent);
        if (!r) r = dir_name(parent, dcl, name);
        if (r) return r;
        uint32_t n = (uint32_t)strlen(name);
        if (n + 1 > pos) return -ENAMETOOLONG;
        pos -= n;
        memcpy(tmp + pos, name, n);
        tmp[--pos] = '/';
        dcl = parent;
    }
    if (pos == sizeof tmp - 1) tmp[--pos] = '/';
    uint32_t len = sizeof tmp - 1 - pos;
    if (len + 1 > size) return -ENAMETOOLONG;
    memcpy(buf, tmp + pos, len + 1);
    return (int)len;
}

// The length of the absolute path of something named n bytes in
// directory dcl (without the NUL), in *len.
static int child_path_len(uint32_t dcl, uint32_t n, uint32_t* len) {
    char where[PATH_MAX];
    int r = dir_path(dcl, where, sizeof where);
    if (r < 0) return r;
    *len = (dcl ? (uint32_t)r + 1 : 1) + n;
    return 0;
}

// The longest path to a directory below top, relative to it ("A/B" is
// 3), in *out. Iterative, with a fixed stack: a legal tree is no
// deeper than PATH_MAX allows, and a deeper one (or a looped chain on
// a corrupt disk) is -ENAMETOOLONG.
static struct { diter_t it; uint32_t len; } subtree[PATH_MAX / 2 + 1];

static int subtree_len(uint32_t top, uint32_t* out) {
    int sp = 0;
    uint32_t best = 0;
    dir_begin(&subtree[0].it, top);
    subtree[0].len = 0;
    while (sp >= 0) {
        dent_t d;
        int r = dir_read(&subtree[sp].it, &d, 0);
        if (r < 0) return r;
        if (!r) { sp--; continue; }
        if (!ent_is_dir(d.e) || ent_is_dot(d.e)) continue;
        if (!valid_cluster(entry_first(d.e))) return -EIO;
        uint32_t len = subtree[sp].len + (subtree[sp].len ? 1 : 0) + (uint32_t)strlen(d.name);
        if (len >= PATH_MAX || sp + 1 == (int)ARRAY_SIZE(subtree)) return -ENAMETOOLONG;
        if (len > best) best = len;
        sp++;
        dir_begin(&subtree[sp].it, entry_first(d.e));
        subtree[sp].len = len;
    }
    *out = best;
    return 0;
}

// ---- Paths ----

// A path walked as far as its last name.
typedef struct {
    uint32_t dir;              // the directory holding that name, or the one named
    int named;                 // 0 when the path ends in a directory: "/", "." or ".."
    char name[NAME_MAX + 1];   // the last name, when named
} path_t;

// One component: 0 for a name (copied to name), 1 for ".", 2 for
// "..", -ENAMETOOLONG or -EINVAL.
static int component(const char* s, size_t n, char name[NAME_MAX + 1]) {
    if (n == 1 && s[0] == '.') return 1;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 2;
    if (n > NAME_MAX) return -ENAMETOOLONG;
    memcpy(name, s, n);
    name[n] = 0;
    return long_name_ok(name) ? 0 : -EINVAL;
}

// Walk path from directory `at`, or from the root when it starts
// with '/'. Every name but the last must be a directory; the last
// need not exist. Repeated and trailing slashes are ignored.
static int resolve(uint32_t at, const char* path, path_t* p) {
    if (!path[0]) return -ENOENT;
    uint32_t d = path[0] == '/' ? 0 : at;
    p->named = 0;
    const char* s = path;
    for (;;) {
        while (*s == '/') s++;
        if (!*s) break;
        const char* t = s;
        while (*t && *t != '/') t++;
        if (p->named) {  // there is more, so the last name was a directory
            dent_t de;
            int r = dir_find(d, p->name, 0, &de);
            if (r < 0) return r;
            if (!r) return -ENOENT;
            if (!ent_is_dir(de.e)) return -ENOTDIR;
            d = entry_first(de.e);
            if (!valid_cluster(d)) return -EIO;
            p->named = 0;
        }
        int k = component(s, (size_t)(t - s), p->name);
        if (k < 0) return k;
        if (k == 2) {
            int r = dir_parent(d, &d);
            if (r) return r;
        }
        p->named = k == 0;
        s = t;
    }
    p->dir = d;
    return 0;
}

// ---- Nodes ----

enum { N_FREE, N_FILE, N_DIR };
static struct { uint32_t key; uint16_t refs; uint8_t kind; } nodes[MAX_NODES] = {
    [DISKFS_ROOT] = { 0, 0, N_DIR },
};

static void nodes_reset(void) {
    memset(nodes, 0, sizeof nodes);
    nodes[DISKFS_ROOT].kind = N_DIR;
}

static int node_find(int kind, uint32_t key) {
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].kind == kind && nodes[i].key == key) return i;
    return -1;
}

// The node for a file (key: its entry's loc) or a directory (key: its
// cluster), with one more reference.
static int node_get(int kind, uint32_t key) {
    int i = node_find(kind, key);
    if (i < 0) {
        for (i = 1; i < MAX_NODES && nodes[i].kind != N_FREE; i++) { }
        if (i == MAX_NODES) return -ENFILE;
        nodes[i].kind = (uint8_t)kind;
        nodes[i].key = key;
        nodes[i].refs = 0;
    }
    if (i != DISKFS_ROOT) nodes[i].refs++;
    return i;
}

static int file_ok(int node) {
    return fs_ok && node > 0 && node < MAX_NODES && nodes[node].kind == N_FILE;
}
static loc_t file_loc(int node) { return nodes[node].key; }

// The directory cluster of node `at`, which path calls start from.
static int start_dir(int at, uint32_t* dcl) {
    if (!fs_ok) return -EIO;
    if (at < 0 || at >= MAX_NODES || nodes[at].kind != N_DIR) return -EBADF;
    *dcl = nodes[at].key;
    return 0;
}

static int resolve_at(int at, const char* path, path_t* p) {
    uint32_t d;
    int r = start_dir(at, &d);
    return r ? r : resolve(d, path, p);
}

// ---- File data ----

// Chain walker: cur is the cluster at index idx, or 0 past the end.
typedef struct { uint32_t idx, cur, prev; } walk_t;

// Move to cluster index ci (never backwards). With extend, missing
// clusters are allocated and linked. Returns the cluster, 0 at the
// end of the chain without extend, -ENOSPC or -EIO.
static int32_t walk_to(walk_t* w, uint8_t* e, uint32_t ci, int extend) {
    for (;;) {
        if (w->cur == 0) {
            if (!extend) return 0;
            uint32_t c = alloc_cluster();
            if (!c) return -ENOSPC;
            if (w->prev) fat_set(w->prev, c);
            else set_first(e, c);
            w->cur = c;
        }
        if (w->idx == ci) return (int32_t)w->cur;
        uint32_t next = fat_get(w->cur);
        w->prev = w->cur;
        w->idx++;
        if (next >= FAT_EOC_MIN) w->cur = 0;
        else if (!valid_cluster(next)) return -EIO;
        else w->cur = next;
    }
}

// Write len bytes at off into the file whose entry copy is e. src
// NULL writes zeros. A gap between the old end and off is
// zero-filled. Returns the bytes of src written, or -errno if none
// were. The caller puts e back.
static int write_range(uint8_t* e, const uint8_t* src, uint32_t len, uint32_t off) {
    if (len == 0) return 0;
    if (off + len < off) return -EFBIG;
    uint32_t size = entry_size(e);
    uint32_t end = off + len;
    uint32_t p = off < size ? off : size;
    int err = 0;
    walk_t w = { 0, entry_first(e), 0 };

    while (p < end) {
        int32_t c = walk_to(&w, e, p / cbytes, 1);
        if (c < 0) { err = c; break; }
        uint32_t in_cl = p % cbytes;
        uint32_t s0 = in_cl % SECTOR;
        uint32_t n = MIN(SECTOR - s0, end - p);
        uint32_t lba = cluster_lba((uint32_t)c) + in_cl / SECTOR;
        if (n != SECTOR) {
            if (p - s0 < size) {
                if (ata_read(lba, 1, secbuf) != 0) { err = -EIO; break; }
            } else {
                memset(secbuf, 0, SECTOR);
            }
        }
        // Bytes before off are the zero-filled gap.
        uint32_t zeros = p < off ? MIN(off - p, n) : 0;
        memset(secbuf + s0, 0, zeros);
        if (src) memcpy(secbuf + s0 + zeros, src + (p + zeros - off), n - zeros);
        else memset(secbuf + s0 + zeros, 0, n - zeros);
        if (ata_write(lba, 1, secbuf) != 0) { err = -EIO; break; }
        p += n;
    }
    if (p > size) set_size(e, p);
    if (p > off) return (int)(p - off);
    return err;
}

// ---- Public API ----

int diskfs_ready(void) { return fs_ok; }

static void release_buffers(void) {
    kfree(fat); kfree(root); kfree(fat_dirty); kfree(root_dirty); kfree(secbuf);
    fat = 0; root = 0; fat_dirty = 0; root_dirty = 0; secbuf = 0;
}

int diskfs_init(void) {
    LOCK();
    fs_ok = 0;
    release_buffers();
    nodes_reset();
    memset(dcache, 0, sizeof dcache);
    int err = 0;
    uint8_t* bs = kmalloc(SECTOR);
    if (!bs) { err = -ENOMEM; goto out; }
    if (!ata_present() || ata_read(0, 1, bs) != 0) { err = -EIO; goto out; }

    uint32_t bps = rd16(bs + 11);
    uint32_t s = bs[13];
    uint32_t res = rd16(bs + 14);
    uint32_t nf = bs[16];
    uint32_t rent = rd16(bs + 17);
    uint32_t tot = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    uint32_t fsz = rd16(bs + 22);
    err = -EINVAL;
    if (bs[510] != 0x55 || bs[511] != 0xAA) goto out;
    if (bps != SECTOR || s == 0 || (s & (s - 1)) || s > 128) goto out;
    if (res == 0 || nf == 0 || nf > MAX_FATS || rent == 0 || fsz == 0) goto out;
    if (tot > ata_sectors()) goto out;
    uint32_t rsec = (rent * 32 + SECTOR - 1) / SECTOR;
    uint32_t dstart = res + nf * fsz + rsec;
    if (dstart >= tot) goto out;
    uint32_t clusters = (tot - dstart) / s;
    if (clusters < 4085 || clusters >= 65525) goto out;  // FAT16 only
    if (fsz * (SECTOR / 2) < clusters + 2) goto out;     // FAT too small

    spc = s; cbytes = s * SECTOR; num_fats = nf; fat_sectors = fsz; fat_start = res;
    root_entries = rent; root_start = res + nf * fsz; root_sectors = rsec;
    data_start = dstart; cluster_count = clusters; free_hint = 2;

    err = -ENOMEM;
    fat = kmalloc(fsz * SECTOR);
    root = kmalloc(rsec * SECTOR);
    fat_dirty = kzalloc(fsz);
    root_dirty = kzalloc(rsec);
    secbuf = kmalloc(SECTOR);
    if (!fat || !root || !fat_dirty || !root_dirty || !secbuf) goto out;
    err = -EIO;
    if (ata_read(fat_start, fsz, fat) != 0) goto out;
    if (ata_read(root_start, rsec, root) != 0) goto out;
    err = 0;
    fs_ok = 1;
out:
    kfree(bs);
    if (err) release_buffers();
    UNLOCK();
    return err;
}

static int lookup_locked(int at, const char* path, int kind) {
    path_t p;
    dent_t d;
    int r = resolve_at(at, path, &p);
    if (r) return r;
    if (!p.named) return kind == N_DIR ? node_get(N_DIR, p.dir) : -EISDIR;
    r = dir_find(p.dir, p.name, 0, &d);
    if (r <= 0) return r < 0 ? r : -ENOENT;
    if (!ent_is_dir(d.e)) return kind == N_FILE ? node_get(N_FILE, d.loc) : -ENOTDIR;
    if (kind == N_FILE) return -EISDIR;
    if (!valid_cluster(entry_first(d.e))) return -EIO;
    return node_get(N_DIR, entry_first(d.e));
}

int diskfs_lookup(int at, const char* path) {
    LOCK();
    int r = lookup_locked(at, path, N_FILE);
    UNLOCK();
    return r;
}

int diskfs_opendir(int at, const char* path) {
    LOCK();
    int r = lookup_locked(at, path, N_DIR);
    UNLOCK();
    return r;
}

// 0 if the path's last name is free to use, -EEXIST, or -errno. A
// name is taken when any entry has it as its long or its short name.
static int name_free(const path_t* p) {
    if (!p->named) return -EEXIST;
    dent_t d;
    int r = dir_find(p->dir, p->name, 1, &d);
    return r < 0 ? r : r ? -EEXIST : 0;
}

// A template entry for dir_add.
static void tmpl_entry(uint8_t e[32], uint8_t attr, uint32_t first, uint32_t size) {
    memset(e, 0, 32);
    e[DIR_ATTR] = attr;
    set_first(e, first);
    set_size(e, size);
}

static int create_locked(int at, const char* path) {
    path_t p;
    loc_t loc;
    uint8_t e[32];
    int r = resolve_at(at, path, &p);
    if (!r) r = name_free(&p);
    if (r) return r;
    tmpl_entry(e, ATTR_ARCHIVE, 0, 0);
    r = dir_add(p.dir, p.name, e, NULL, &loc);
    int c = commit();
    if (r || c) return r ? r : c;
    return node_get(N_FILE, loc);
}

int diskfs_create(int at, const char* path) {
    LOCK();
    int r = create_locked(at, path);
    UNLOCK();
    return r;
}

static int mkdir_locked(int at, const char* path) {
    path_t p;
    loc_t loc;
    uint8_t e[32];
    uint32_t len;
    int r = resolve_at(at, path, &p);
    if (!r) r = name_free(&p);
    if (!r) r = child_path_len(p.dir, (uint32_t)strlen(p.name), &len);
    if (r) return r;
    // Every directory's full path must fit PATH_MAX, or getcwd fails.
    if (len >= PATH_MAX) return -ENAMETOOLONG;
    uint32_t c = alloc_cluster();
    if (!c) return -ENOSPC;
    r = init_dir_cluster(c, 1, p.dir);
    if (!r) {
        tmpl_entry(e, ATTR_DIRECTORY, c, 0);
        r = dir_add(p.dir, p.name, e, NULL, &loc);
    }
    if (r) fat_set(c, 0);
    int cr = commit();
    return r ? r : cr;
}

int diskfs_mkdir(int at, const char* path) {
    LOCK();
    int r = mkdir_locked(at, path);
    UNLOCK();
    return r;
}

// unlink (dir 0) and rmdir (dir 1).
static int remove_locked(int at, const char* path, int dir) {
    path_t p;
    dent_t d;
    int r = resolve_at(at, path, &p);
    if (r) return r;
    if (!p.named) return dir ? -EBUSY : -EISDIR;  // ".", ".." or "/"
    r = dir_find(p.dir, p.name, 0, &d);
    if (r <= 0) return r < 0 ? r : -ENOENT;
    if (ent_is_dir(d.e) != dir) return dir ? -ENOTDIR : -EISDIR;
    uint32_t first = entry_first(d.e);
    if (dir) {
        if (!valid_cluster(first)) return -EIO;
        if (node_find(N_DIR, first) >= 0) return -EBUSY;  // someone's current directory
        r = dir_empty(first);
        if (r <= 0) return r < 0 ? r : -ENOTEMPTY;
    } else if (node_find(N_FILE, d.loc) >= 0) {
        return -EBUSY;  // open
    }
    free_chain(first);
    r = dir_remove(&d);
    int c = commit();
    return r ? r : c;
}

int diskfs_unlink(int at, const char* path) {
    LOCK();
    int r = remove_locked(at, path, 0);
    UNLOCK();
    return r;
}

int diskfs_rmdir(int at, const char* path) {
    LOCK();
    int r = remove_locked(at, path, 1);
    UNLOCK();
    return r;
}

static int rename_locked(int at, const char* from, const char* to) {
    path_t s, t;
    dent_t sd, td;
    const uint8_t* keep = NULL;
    int r = resolve_at(at, from, &s);
    if (r) return r;
    if (!s.named) return -EBUSY;  // ".", ".." or "/"
    r = dir_find(s.dir, s.name, 0, &sd);
    if (r <= 0) return r < 0 ? r : -ENOENT;
    r = resolve_at(at, to, &t);
    if (r) return r;
    if (!t.named) return -EEXIST;
    r = dir_find(t.dir, t.name, 1, &td);
    if (r < 0) return r;
    if (r) {
        // Taken, unless by the entry itself: then only the case or the
        // spelling (its short name) changes, or nothing does.
        if (t.dir != s.dir || td.loc != sd.loc) return -EEXIST;
        if (strcmp(sd.name, t.name) == 0) return 0;
        if (strcasecmp(sd.name, t.name) == 0) keep = sd.e;
    }
    int isdir = ent_is_dir(sd.e);
    uint32_t c = entry_first(sd.e);
    loc_t dotdot = 0;
    if (isdir) {
        if (!valid_cluster(c)) return -EIO;
        // Not into itself or anything below it.
        uint32_t up = t.dir;
        for (uint32_t n = 0; up; n++) {
            if (up == c) return -EINVAL;
            if (n > cluster_count) return -EIO;
            r = dir_parent(up, &up);
            if (r) return r;
        }
        uint32_t len, below;
        r = child_path_len(t.dir, (uint32_t)strlen(t.name), &len);
        if (!r) r = subtree_len(c, &below);
        if (r) return r;
        if (len + (below ? 1 + below : 0) >= PATH_MAX) return -ENAMETOOLONG;
        if (t.dir != s.dir) {
            if (dir_find(c, "..", 0, &td) <= 0) return -EIO;
            dotdot = td.loc;
        }
    }
    // The new entry goes in first, then the old one goes away. A
    // moved directory's ".." follows it.
    loc_t nloc;
    r = dir_add(t.dir, t.name, sd.e, keep, &nloc);
    if (!r) {
        r = dir_remove(&sd);
        if (r) {
            uint8_t x[32];
            if (!ent_get(nloc, x)) { x[0] = 0xE5; ent_put(nloc, x); }
        }
    }
    if (!r && dotdot) {
        uint8_t dd[32];
        r = ent_get(dotdot, dd);
        if (!r) { set_first(dd, t.dir); r = ent_put(dotdot, dd); }
    }
    if (!r && !isdir) {
        int n = node_find(N_FILE, sd.loc);
        if (n >= 0) nodes[n].key = nloc;  // an open file stays open
    }
    int cr = commit();
    return r ? r : cr;
}

int diskfs_rename(int at, const char* from, const char* to) {
    LOCK();
    int r = rename_locked(at, from, to);
    UNLOCK();
    return r;
}

#define DIR_ID 0x80000000u  // dirent_t.id of a directory: its cluster | this

static int stat_locked(int at, const char* path, dirent_t* d) {
    path_t p;
    dent_t de;
    int r = resolve_at(at, path, &p);
    if (r) return r;
    memset(d, 0, sizeof *d);
    if (!p.named) {  // a directory named by "/", "." or ".."
        d->flags = DIRENT_DIR;
        d->id = DIR_ID | p.dir;
        if (!p.dir) { d->name[0] = '/'; return 0; }
        uint32_t parent;
        r = dir_parent(p.dir, &parent);
        return r ? r : dir_name(parent, p.dir, d->name);
    }
    r = dir_find(p.dir, p.name, 0, &de);
    if (r <= 0) return r < 0 ? r : -ENOENT;
    memcpy(d->name, de.name, strlen(de.name) + 1);
    if (ent_is_dir(de.e)) {
        d->flags = DIRENT_DIR;
        d->id = DIR_ID | entry_first(de.e);
    } else {
        d->size = entry_size(de.e);
        d->id = de.loc;
    }
    return 0;
}

int diskfs_stat(int at, const char* path, dirent_t* d) {
    LOCK();
    int r = stat_locked(at, path, d);
    UNLOCK();
    return r;
}

uint32_t diskfs_size(int node) {
    LOCK();
    uint8_t e[32];
    uint32_t s = file_ok(node) && ent_get(file_loc(node), e) == 0 ? entry_size(e) : 0;
    UNLOCK();
    return s;
}

int diskfs_pread(int node, void* buf, uint32_t len, uint32_t off) {
    LOCK();
    uint8_t e[32];
    if (!file_ok(node)) { UNLOCK(); return -EBADF; }
    if (ent_get(file_loc(node), e)) { UNLOCK(); return -EIO; }
    uint32_t size = entry_size(e);
    if (off >= size) { UNLOCK(); return 0; }
    if (len > size - off) len = size - off;
    uint8_t* out = buf;
    uint32_t done = 0;
    int err = 0;
    walk_t w = { 0, entry_first(e), 0 };
    while (done < len) {
        uint32_t p = off + done;
        int32_t c = walk_to(&w, e, p / cbytes, 0);
        if (c <= 0) { err = -EIO; break; }  // chain shorter than the size says
        uint32_t in_cl = p % cbytes;
        uint32_t s0 = in_cl % SECTOR;
        uint32_t n = MIN(SECTOR - s0, len - done);
        uint32_t lba = cluster_lba((uint32_t)c) + in_cl / SECTOR;
        if (n == SECTOR) {
            if (ata_read(lba, 1, out + done) != 0) { err = -EIO; break; }
        } else {
            if (ata_read(lba, 1, secbuf) != 0) { err = -EIO; break; }
            memcpy(out + done, secbuf + s0, n);
        }
        done += n;
    }
    UNLOCK();
    return done ? (int)done : err;
}

int diskfs_pwrite(int node, const void* buf, uint32_t len, uint32_t off) {
    LOCK();
    uint8_t e[32];
    int r;
    if (!file_ok(node)) r = -EBADF;
    else if (ent_get(file_loc(node), e)) r = -EIO;
    else {
        r = write_range(e, buf, len, off);
        int err = ent_put(file_loc(node), e);
        int c = commit();
        if (!err) err = c;
        if (err && r >= 0) r = err;
    }
    UNLOCK();
    return r;
}

static int truncate_locked(uint8_t* e, uint32_t len) {
    uint32_t size = entry_size(e);
    if (len > size) {
        int r = write_range(e, 0, len - size, size);
        if (r < 0) return r;
        return entry_size(e) == len ? 0 : -ENOSPC;
    }
    if (len == size) return 0;
    uint32_t keep = (len + cbytes - 1) / cbytes;
    uint32_t first = entry_first(e);
    if (keep == 0) {
        free_chain(first);
        set_first(e, 0);
    } else {
        walk_t w = { 0, first, 0 };
        int32_t last = walk_to(&w, e, keep - 1, 0);
        if (last <= 0) return -EIO;
        uint32_t next = fat_get((uint32_t)last);
        fat_set((uint32_t)last, FAT_EOC);
        if (next < FAT_EOC_MIN) free_chain(next);
    }
    set_size(e, len);
    return 0;
}

int diskfs_truncate(int node, uint32_t len) {
    LOCK();
    uint8_t e[32];
    int r;
    if (!file_ok(node)) r = -EBADF;
    else if (ent_get(file_loc(node), e)) r = -EIO;
    else {
        r = truncate_locked(e, len);
        int err = ent_put(file_loc(node), e);
        int c = commit();
        if (!err) err = c;
        if (!r) r = err;
    }
    UNLOCK();
    return r;
}

static int write_file_locked(int at, const char* path, const void* buf, uint32_t len) {
    path_t p;
    dent_t d;
    int r = resolve_at(at, path, &p);
    if (r) return r;
    if (!p.named) return -EISDIR;
    r = dir_find(p.dir, p.name, 1, &d);
    if (r < 0) return r;
    int created = !r;
    if (r && !ent_usable(d.e)) return -EEXIST;  // the volume label, say
    if (r && ent_is_dir(d.e)) return -EISDIR;
    // Build the new chain on a scratch entry; the real one switches
    // over (or is made) only once all of it is written.
    uint8_t tmp[32];
    memset(tmp, 0, sizeof tmp);
    int w = write_range(tmp, buf, len, 0);
    if (w < 0 || (uint32_t)w != len) {
        free_chain(entry_first(tmp));
        commit();
        return w < 0 ? w : -ENOSPC;
    }
    if (created) {
        uint8_t e[32];
        loc_t loc;
        tmpl_entry(e, ATTR_ARCHIVE, entry_first(tmp), len);
        r = dir_add(p.dir, p.name, e, NULL, &loc);
        if (r) free_chain(entry_first(tmp));
    } else {
        uint32_t old = entry_first(d.e);
        set_first(d.e, entry_first(tmp));
        set_size(d.e, len);
        r = ent_put(d.loc, d.e);
        free_chain(r ? entry_first(tmp) : old);
    }
    int c = commit();
    return r ? r : c;
}

int diskfs_write_file(int at, const char* path, const void* buf, uint32_t len) {
    LOCK();
    int r = write_file_locked(at, path, buf, len);
    UNLOCK();
    return r;
}

int diskfs_readdir(int dir, int* cursor, dirent_t* d) {
    LOCK();
    uint32_t dcl;
    int r = start_dir(dir, &dcl);
    if (!r && *cursor < 0) r = -ENOENT;
    if (!r) {
        // Skip the slots listed already, then read on from there.
        diter_t it;
        dir_begin(&it, dcl);
        loc_t l;
        int k = 1;
        while (it.n < (uint32_t)*cursor && (k = dir_next(&it, &l)) > 0) { }
        dent_t de;
        while (k > 0 && (k = dir_read(&it, &de, 0)) > 0 && ent_is_dot(de.e)) { }
        if (k < 0) r = k;
        else if (k == 0) r = -ENOENT;
        else {
            memset(d, 0, sizeof *d);
            memcpy(d->name, de.name, strlen(de.name) + 1);
            d->flags = ent_is_dir(de.e) ? DIRENT_DIR : 0;
            d->size = ent_is_dir(de.e) ? 0 : entry_size(de.e);
            d->id = ent_is_dir(de.e) ? DIR_ID | entry_first(de.e) : de.loc;
            *cursor = (int)it.n;
        }
    }
    UNLOCK();
    return r;
}

int diskfs_getcwd(int dir, char* buf, uint32_t size) {
    LOCK();
    uint32_t dcl;
    int r = start_dir(dir, &dcl);
    if (!r) r = dir_path(dcl, buf, size);
    UNLOCK();
    return r;
}

void diskfs_ref(int node) {
    LOCK();
    if (node > 0 && node < MAX_NODES && nodes[node].kind != N_FREE) nodes[node].refs++;
    UNLOCK();
}

void diskfs_unref(int node) {
    LOCK();
    if (node > 0 && node < MAX_NODES && nodes[node].kind != N_FREE && nodes[node].refs)
        if (--nodes[node].refs == 0) nodes[node].kind = N_FREE;
    UNLOCK();
}

int diskfs_refs(int node) {
    LOCK();
    int r = node > 0 && node < MAX_NODES && nodes[node].kind != N_FREE ? nodes[node].refs : 0;
    UNLOCK();
    return r;
}

void diskfs_stats(uint32_t* clusters, uint32_t* free_clusters, uint32_t* cluster_bytes) {
    LOCK();
    uint32_t f = 0;
    if (fs_ok)
        for (uint32_t c = 2; c < cluster_count + 2; c++)
            if (fat_get(c) == 0) f++;
    if (clusters) *clusters = fs_ok ? cluster_count : 0;
    if (free_clusters) *free_clusters = f;
    if (cluster_bytes) *cluster_bytes = fs_ok ? cbytes : 0;
    UNLOCK();
}

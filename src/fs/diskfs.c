// FAT16 filesystem on the primary ATA disk, root directory only.
//
// Layout (all derived from the BPB in sector 0, so images made by
// other tools work): reserved sectors, the FAT copies, the fixed
// root directory, then the data area where cluster 2 is the first
// data cluster. Names are DOS 8.3, stored uppercase.
//
// The FAT and root directory are held in memory. Mutations mark the
// sectors they touch dirty, and commit() writes those back (every
// FAT copy, then the directory) and flushes the disk cache. File
// data is written before the metadata that points at it, so a crash
// can leak clusters but not expose another file's data.
//
// Not supported: long file names (their entries are skipped),
// subdirectories, timestamps, FAT12 and FAT32.
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
static uint16_t* refs;          // open count per root entry
static uint8_t* secbuf;         // scratch sector, used under the lock

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// ---- Names ----

static int bad_char(char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) return 1;
    return strchr(" \"*+,/:;<=>?[\\]|.", c) != 0;
}

// User name to the 11-byte raw form. 0 on success, -1 if it does not
// fit 8.3 or uses a character FAT forbids. Lowercase is folded.
static int name_to_raw(const char* name, uint8_t raw[11]) {
    if (!name) return -1;
    const char* dot = strchr(name, '.');
    size_t len = strlen(name);
    size_t base = dot ? (size_t)(dot - name) : len;
    size_t ext = dot ? len - base - 1 : 0;
    if (base < 1 || base > 8 || ext > 3) return -1;
    if (dot && ext == 0) return -1;  // "NAME." is not a name
    memset(raw, ' ', 11);
    for (size_t i = 0; i < base; i++) {
        char c = name[i];
        if (bad_char(c)) return -1;
        raw[i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    for (size_t i = 0; i < ext; i++) {
        char c = dot[1 + i];
        if (bad_char(c)) return -1;
        raw[8 + i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    return 0;
}

static void raw_to_display(const uint8_t raw[11], char out[FS_NAME_MAX]) {
    int b = 8, e = 3, o = 0;
    while (b > 0 && raw[b - 1] == ' ') b--;
    while (e > 0 && raw[8 + e - 1] == ' ') e--;
    for (int i = 0; i < b; i++) out[o++] = (char)raw[i];
    if (e > 0) {
        out[o++] = '.';
        for (int i = 0; i < e; i++) out[o++] = (char)raw[8 + i];
    }
    out[o] = 0;
}

int diskfs_name_ok(const char* name) {
    uint8_t raw[11];
    return name_to_raw(name, raw) == 0;
}

// ---- Directory ----

static uint8_t* entry(uint32_t i) { return root + i * 32; }
static uint32_t entry_first(const uint8_t* e) { return rd16(e + DIR_FIRST_LOW); }
static uint32_t entry_size(const uint8_t* e) { return rd32(e + DIR_SIZE); }

static void entry_dirty(const uint8_t* e) {
    if (e >= root && e < root + root_entries * 32)
        root_dirty[(uint32_t)(e - root) / SECTOR] = 1;
}
static void set_first(uint8_t* e, uint32_t c) { wr16(e + DIR_FIRST_LOW, (uint16_t)c); entry_dirty(e); }
static void set_size(uint8_t* e, uint32_t s) { wr32(e + DIR_SIZE, s); entry_dirty(e); }

static int is_file(const uint8_t* e) {
    if (e[0] == 0x00 || e[0] == 0xE5) return 0;
    if (e[DIR_ATTR] == ATTR_LFN) return 0;
    if (e[DIR_ATTR] & (ATTR_DIRECTORY | ATTR_VOLUME)) return 0;
    return rd16(e + DIR_FIRST_HIGH) == 0;  // FAT32 leftovers are not ours
}

static int find_raw(const uint8_t raw[11]) {
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry(i);
        if (e[0] == 0x00) break;  // end of directory
        if (is_file(e) && memcmp(e, raw, 11) == 0) return (int)i;
    }
    return -1;
}

// Any live entry with this name, including directories and labels.
static int name_taken(const uint8_t raw[11]) {
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry(i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[DIR_ATTR] == ATTR_LFN) continue;
        if (memcmp(e, raw, 11) == 0) return 1;
    }
    return 0;
}

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

// Bounded, so a looped chain on a corrupt disk cannot hang us.
static void free_chain(uint32_t c) {
    for (uint32_t steps = 0; valid_cluster(c) && steps < cluster_count; steps++) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if (c < free_hint) free_hint = c;
        if (next >= FAT_EOC_MIN) break;
        c = next;
    }
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
    if (ata_flush() != 0) err = -EIO;
    return err;
}

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

// Write len bytes at off into the file described by entry e (which
// may be a scratch entry outside the directory). src NULL writes
// zeros. A gap between the old end and off is zero-filled. Returns
// the bytes of src written, or -errno if none were.
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
    kfree(fat); kfree(root); kfree(fat_dirty); kfree(root_dirty); kfree(refs); kfree(secbuf);
    fat = 0; root = 0; fat_dirty = 0; root_dirty = 0; refs = 0; secbuf = 0;
}

int diskfs_init(void) {
    LOCK();
    fs_ok = 0;
    release_buffers();
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
    refs = kzalloc(rent * sizeof(uint16_t));
    secbuf = kmalloc(SECTOR);
    if (!fat || !root || !fat_dirty || !root_dirty || !refs || !secbuf) goto out;
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

int diskfs_lookup(const char* name) {
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) return -EINVAL;
    LOCK();
    int r = fs_ok ? find_raw(raw) : -EIO;
    if (r == -1) r = -ENOENT;
    UNLOCK();
    return r;
}

static int create_locked(const uint8_t raw[11]) {
    if (name_taken(raw)) return -EEXIST;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t* e = entry(i);
        if (e[0] != 0x00 && e[0] != 0xE5) continue;
        memset(e, 0, 32);
        memcpy(e, raw, 11);
        e[DIR_ATTR] = ATTR_ARCHIVE;
        entry_dirty(e);
        refs[i] = 0;
        int err = commit();
        return err ? err : (int)i;
    }
    return -ENOSPC;
}

int diskfs_create(const char* name) {
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) return -EINVAL;
    LOCK();
    int r = fs_ok ? create_locked(raw) : -EIO;
    UNLOCK();
    return r;
}

int diskfs_unlink(const char* name) {
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) return -EINVAL;
    LOCK();
    int r;
    int i = fs_ok ? find_raw(raw) : -2;
    if (i == -2) r = -EIO;
    else if (i < 0) r = -ENOENT;
    else if (refs[i]) r = -EBUSY;
    else {
        uint8_t* e = entry((uint32_t)i);
        free_chain(entry_first(e));
        e[0] = 0xE5;
        entry_dirty(e);
        r = commit();
    }
    UNLOCK();
    return r;
}

static int node_ok(int node) {
    return fs_ok && node >= 0 && (uint32_t)node < root_entries && is_file(entry((uint32_t)node));
}

uint32_t diskfs_size(int node) {
    LOCK();
    uint32_t s = node_ok(node) ? entry_size(entry((uint32_t)node)) : 0;
    UNLOCK();
    return s;
}

int diskfs_pread(int node, void* buf, uint32_t len, uint32_t off) {
    LOCK();
    if (!node_ok(node)) { UNLOCK(); return -EBADF; }
    uint8_t* e = entry((uint32_t)node);
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
    int r;
    if (!node_ok(node)) r = -EBADF;
    else {
        r = write_range(entry((uint32_t)node), buf, len, off);
        int err = commit();
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
    int r;
    if (!node_ok(node)) r = -EBADF;
    else {
        r = truncate_locked(entry((uint32_t)node), len);
        int err = commit();
        if (!r) r = err;
    }
    UNLOCK();
    return r;
}

int diskfs_write_file(const char* name, const void* buf, uint32_t len) {
    uint8_t raw[11];
    if (name_to_raw(name, raw) != 0) return -EINVAL;
    LOCK();
    int r = 0;
    if (!fs_ok) { r = -EIO; goto out; }
    int i = find_raw(raw), created = 0;
    if (i < 0) {
        i = create_locked(raw);
        if (i < 0) { r = i; goto out; }
        created = 1;
    }
    // Build the new chain on a scratch entry, then switch over.
    uint8_t tmp[32];
    memset(tmp, 0, sizeof tmp);
    int w = write_range(tmp, buf, len, 0);
    if (w < 0 || (uint32_t)w != len) {
        free_chain(entry_first(tmp));
        if (created) {  // leave no empty file behind
            entry((uint32_t)i)[0] = 0xE5;
            entry_dirty(entry((uint32_t)i));
        }
        commit();
        r = w < 0 ? w : -ENOSPC;
        goto out;
    }
    uint8_t* e = entry((uint32_t)i);
    uint32_t old = entry_first(e);
    set_first(e, entry_first(tmp));
    set_size(e, len);
    free_chain(old);
    r = commit();
out:
    UNLOCK();
    return r;
}

int diskfs_readdir(int* cursor, char name[FS_NAME_MAX], uint32_t* size) {
    LOCK();
    int r = -ENOENT;
    for (uint32_t i = (uint32_t)*cursor; fs_ok && *cursor >= 0 && i < root_entries; i++) {
        uint8_t* e = entry(i);
        if (e[0] == 0x00) break;
        if (!is_file(e)) continue;
        raw_to_display(e, name);
        if (size) *size = entry_size(e);
        *cursor = (int)i + 1;
        r = 0;
        break;
    }
    UNLOCK();
    return r;
}

void diskfs_ref(int node) {
    LOCK();
    if (node_ok(node)) refs[node]++;
    UNLOCK();
}

void diskfs_unref(int node) {
    LOCK();
    if (fs_ok && node >= 0 && (uint32_t)node < root_entries && refs[node]) refs[node]--;
    UNLOCK();
}

int diskfs_refs(int node) {
    LOCK();
    int r = node_ok(node) ? refs[node] : 0;
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

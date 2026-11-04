// FAT16 filesystem on the primary ATA disk (diskfs.c) and the
// per-task file descriptor layer on top of it (file.c).
#ifndef RINGTAIL_FS_H
#define RINGTAIL_FS_H

#include <stdint.h>

#define FS_NAME_MAX 13  // "NAME8CHR.EXT" plus NUL

// A node is a file's index in the root directory. It stays valid
// while the file is referenced (open), because a referenced file
// cannot be deleted.
int diskfs_init(void);           // 0, or -errno when there is no usable disk
int diskfs_ready(void);
int diskfs_lookup(const char* name);         // node or -errno
int diskfs_create(const char* name);         // node, -EEXIST if it exists
int diskfs_unlink(const char* name);         // 0, -EBUSY while open
uint32_t diskfs_size(int node);
int diskfs_pread(int node, void* buf, uint32_t len, uint32_t off);
int diskfs_pwrite(int node, const void* buf, uint32_t len, uint32_t off);
int diskfs_truncate(int node, uint32_t len);
// Replace a file's contents in one step, creating it if needed.
// The new data is written to fresh clusters before the directory
// entry is switched, so a failed write leaves the old file intact.
int diskfs_write_file(const char* name, const void* buf, uint32_t len);
// Iterate the directory. *cursor starts at 0. Returns 0 and fills
// name and size, or -ENOENT after the last file.
int diskfs_readdir(int* cursor, char name[FS_NAME_MAX], uint32_t* size);
void diskfs_ref(int node);
void diskfs_unref(int node);
int diskfs_refs(int node);
void diskfs_stats(uint32_t* clusters, uint32_t* free_clusters, uint32_t* cluster_bytes);
// 1 if name is a valid DOS 8.3 name, 0 if not. Does not touch the disk.
int diskfs_name_ok(const char* name);

// ---- File descriptors ----
#define MAX_FDS 16

typedef enum { FD_NONE = 0, FD_CONSOLE_IN, FD_CONSOLE_OUT, FD_FILE } fd_kind_t;

typedef struct {
    fd_kind_t kind;
    int flags;
    int node;
    uint32_t offset;
} fd_t;

struct task;
void files_init_std(struct task* t);
void files_close_all(struct task* t);
// These operate on the current task and take user pointers.
int file_open(const char* uname, int flags);
int file_close(int fd);
int file_read(int fd, void* ubuf, uint32_t len);
int file_write(int fd, const void* ubuf, uint32_t len);
int file_seek(int fd, int32_t off, int whence);

#endif

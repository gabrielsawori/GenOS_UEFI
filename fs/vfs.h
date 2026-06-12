#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * GenOS Virtual File System (VFS) Layer
 *
 * Provides a POSIX-like file descriptor abstraction over two backends:
 *   - TAR ramdisk (read-only, cached via buffer cache)
 *   - tmpfs (read/write, in-memory heap storage)
 *
 * Each process gets its own FD table (embedded in struct task).
 */

#define VFS_MAX_FDS   16   /* Per-process FD limit */
#define VFS_MAX_FILES 64   /* Global open file limit */

/* Seek modes */
#define VFS_SEEK_SET  0
#define VFS_SEEK_CUR  1
#define VFS_SEEK_END  2

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREATE  4   /* Create if not exists (goes to tmpfs) */

typedef enum {
    VFS_TYPE_NONE = 0,
    VFS_TYPE_FILE,
    VFS_TYPE_DIR
} vfs_type_t;

/*
 * Per-process file descriptor.
 * Stored directly in struct task (no heap allocation needed).
 */
typedef struct {
    vfs_type_t type;
    char       name[100];   /* Filename (for display/debug) */
    uint8_t*   data;        /* Pointer to file data (TAR) or NULL (tmpfs) */
    size_t     size;        /* File size in bytes (0 for dir) */
    size_t     offset;      /* Current read/write position */
    int        writable;    /* 1 = tmpfs-backed (can write), 0 = TAR (read-only) */
    int        tmpfs_idx;   /* Index in tmpfs table (>=0) or -1 for TAR files */
} vfs_fd_t;

/* Initialize VFS subsystem (call once at boot) */
void vfs_init(void);

/* Open a file or directory; returns FD number (>=0) or -1 on error */
int vfs_open(uint32_t pid, const char* filename, int flags);

/* Read up to count bytes from fd into buf; returns bytes read or -1 */
int vfs_read(uint32_t pid, int fd, void* buf, size_t count);

/* Close a file descriptor; returns 0 on success, -1 on error */
int vfs_close(uint32_t pid, int fd);

/* Seek to a new position; returns new offset or -1 */
int vfs_seek(uint32_t pid, int fd, int64_t offset, int whence);

/* Read next directory entry; returns 1 if entry found, 0 when done */
int vfs_readdir(uint32_t pid, int fd, char* name_buf, size_t* size_buf);

/* Write up to count bytes to fd from buf; returns bytes written or -1 */
int vfs_write(uint32_t pid, int fd, const void* buf, size_t count);

/* Create a new file in tmpfs; returns 0 on success, -1 on error */
int vfs_create(uint32_t pid, const char* filename);

/* Delete a file from tmpfs; returns 0 on success, -1 on error */
int vfs_unlink(uint32_t pid, const char* filename);

#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * GenOS tmpfs - In-Memory Writeable Filesystem
 *
 * Provides a simple file storage in kernel heap memory.
 * Files can be created, written, read, and deleted at runtime.
 * Complements the read-only TAR ramdisk backend.
 *
 * Limits:
 *   TMPFS_MAX_FILES = 32 files
 *   TMPFS_MAX_SIZE  = 64KB per file
 */

#define TMPFS_MAX_FILES  32
#define TMPFS_MAX_SIZE   65536   /* 64KB per file */

typedef struct {
    int      in_use;
    char     name[100];
    uint8_t* data;       /* kmalloc'd buffer */
    size_t   size;        /* Current used bytes */
    size_t   capacity;    /* Allocated buffer size */
} tmpfs_file_t;

/* Initialize tmpfs subsystem */
void tmpfs_init(void);

/* Look up a file by name; returns index (>=0) or -1 if not found */
int tmpfs_open(const char* name);

/* Create a new empty file; returns index (>=0) or -1 on error */
int tmpfs_create(const char* name);

/*
 * Write `count` bytes from `buf` to file at `offset`.
 * Grows buffer via krealloc if needed (up to TMPFS_MAX_SIZE).
 * Returns bytes written, or -1 on error.
 */
int tmpfs_write(int index, size_t offset, const void* buf, size_t count);

/*
 * Read up to `count` bytes from file at `offset` into `buf`.
 * Returns bytes read, or -1 on error.
 */
int tmpfs_read(int index, size_t offset, void* buf, size_t count);

/* Delete a file by name; frees its buffer. Returns 0 or -1 */
int tmpfs_unlink(const char* name);

/*
 * Get entry by global index (for readdir iteration).
 * Iterates both TAR (indices 0..N-1) and tmpfs (indices N..N+M-1).
 * For tmpfs-only iteration, use tmpfs_index >= tar_entry_count.
 * Returns 1 if entry exists, 0 if past end.
 */
int tmpfs_get_entry(int index, char* name_buf, size_t* size_buf);

/* Get count of tmpfs files (for readdir boundary) */
int tmpfs_file_count(void);

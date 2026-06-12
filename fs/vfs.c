/*
 * GenOS Virtual File System (VFS) Implementation
 *
 * Routes file operations to two backends:
 *   - TAR ramdisk (read-only, accelerated by buffer cache)
 *   - tmpfs (read/write, stored in kernel heap)
 *
 * FD tables live inside struct task (per-process).
 */
#include "vfs.h"
#include "tar.h"
#include "tmpfs.h"
#include "cache.h"
#include "../drivers/serial.h"
#include "../kernel/task.h"
#include "../libc/string.h"

/*
 * External helper from task.c: returns the current task's FD array.
 * Always returns the running task's fds[] since syscalls execute in
 * the calling task's context.
 */
extern vfs_fd_t* task_get_current_fds(void);

/* ─── Initialization ────────────────────────────────────────────── */

void vfs_init(void) {
    cache_init();
    tmpfs_init();
    serial_write_string("[OK] VFS layer initialized.\n");
}

/* ─── Open ─────────────────────────────────────────────────────── */

int vfs_open(uint32_t pid, const char* filename, int flags) {
    (void)pid;
    if (!filename) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    /* Special case: "/" opens a directory listing */
    if (filename[0] == '/' && filename[1] == '\0') {
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (fds[i].type == VFS_TYPE_NONE) {
                fds[i].type      = VFS_TYPE_DIR;
                fds[i].name[0]   = '/';
                fds[i].name[1]   = '\0';
                fds[i].data      = NULL;
                fds[i].size      = 0;
                fds[i].offset    = 0;
                fds[i].writable  = 0;
                fds[i].tmpfs_idx = -1;
                return i;
            }
        }
        return -1;
    }

    /* Check tmpfs first (writeable backend) */
    int tidx = tmpfs_open(filename);
    if (tidx >= 0) {
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (fds[i].type == VFS_TYPE_NONE) {
                fds[i].type      = VFS_TYPE_FILE;
                fds[i].writable  = 1;
                fds[i].tmpfs_idx = tidx;
                fds[i].data      = NULL;
                fds[i].offset    = 0;
                /* Get size from tmpfs */
                char tmp_name[100];
                size_t tmp_size = 0;
                tmpfs_get_entry(tidx, tmp_name, &tmp_size);
                fds[i].size = tmp_size;
                /* Copy filename */
                int j;
                for (j = 0; j < 99 && filename[j] != '\0'; j++) {
                    fds[i].name[j] = filename[j];
                }
                fds[i].name[j] = '\0';
                return i;
            }
        }
        return -1;
    }

    /* Fall back to TAR backend (read-only) */
    size_t file_size = 0;
    char* file_data = tar_read_file(filename, &file_size);

    if (!file_data) {
        /* File not found in either backend */
        if (flags & O_CREATE) {
            /* Create in tmpfs */
            tidx = tmpfs_create(filename);
            if (tidx < 0) return -1;
            for (int i = 0; i < VFS_MAX_FDS; i++) {
                if (fds[i].type == VFS_TYPE_NONE) {
                    fds[i].type      = VFS_TYPE_FILE;
                    fds[i].writable  = 1;
                    fds[i].tmpfs_idx = tidx;
                    fds[i].data      = NULL;
                    fds[i].size      = 0;
                    fds[i].offset    = 0;
                    int j;
                    for (j = 0; j < 99 && filename[j] != '\0'; j++) {
                        fds[i].name[j] = filename[j];
                    }
                    fds[i].name[j] = '\0';
                    return i;
                }
            }
            return -1;
        }
        return -1;
    }

    /* Found in TAR — open as read-only */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (fds[i].type == VFS_TYPE_NONE) {
            fds[i].type      = VFS_TYPE_FILE;
            fds[i].writable  = 0;
            fds[i].tmpfs_idx = -1;
            int j;
            for (j = 0; j < 99 && filename[j] != '\0'; j++) {
                fds[i].name[j] = filename[j];
            }
            fds[i].name[j] = '\0';
            fds[i].data   = (uint8_t*)file_data;
            fds[i].size   = file_size;
            fds[i].offset = 0;
            return i;
        }
    }
    return -1;
}

/* ─── Read ─────────────────────────────────────────────────────── */

int vfs_read(uint32_t pid, int fd, void* buf, size_t count) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS || !buf) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    vfs_fd_t* f = &fds[fd];
    if (f->type != VFS_TYPE_FILE) return -1;

    /* Already at end of file */
    if (f->offset >= f->size) return 0;

    int result;
    if (f->tmpfs_idx >= 0) {
        /* tmpfs-backed file: read directly from tmpfs */
        result = tmpfs_read(f->tmpfs_idx, f->offset, buf, count);
    } else {
        /* TAR-backed file: read through buffer cache */
        result = cache_read(f->name, f->data, f->size, f->offset, buf, count);
    }

    if (result > 0) {
        f->offset += (size_t)result;
    }
    return result;
}

/* ─── Close ────────────────────────────────────────────────────── */

int vfs_close(uint32_t pid, int fd) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    if (fds[fd].type == VFS_TYPE_NONE) return -1;

    /* Mark slot as free */
    fds[fd].type      = VFS_TYPE_NONE;
    fds[fd].writable  = 0;
    fds[fd].tmpfs_idx = -1;
    return 0;
}

/* ─── Seek ─────────────────────────────────────────────────────── */

int vfs_seek(uint32_t pid, int fd, int64_t offset, int whence) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    vfs_fd_t* f = &fds[fd];
    if (f->type != VFS_TYPE_FILE) return -1;

    int64_t new_pos;
    switch (whence) {
        case VFS_SEEK_SET: new_pos = offset;                       break;
        case VFS_SEEK_CUR: new_pos = (int64_t)f->offset + offset; break;
        case VFS_SEEK_END: new_pos = (int64_t)f->size + offset;   break;
        default: return -1;
    }

    /* Clamp to valid range [0, size] */
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int64_t)f->size) new_pos = (int64_t)f->size;

    f->offset = (size_t)new_pos;
    return (int)f->offset;
}

/* ─── Read Directory Entry ─────────────────────────────────────── */

/*
 * Count total TAR entries by walking the archive.
 * Used by readdir to determine boundary between TAR and tmpfs listings.
 */
static int tar_entry_count(void) {
    int count = 0;
    char name_buf[100];
    size_t size_buf = 0;
    while (tar_get_entry(count, name_buf, &size_buf)) {
        count++;
    }
    return count;
}

int vfs_readdir(uint32_t pid, int fd, char* name_buf, size_t* size_buf) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS || !name_buf || !size_buf) return 0;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return 0;

    vfs_fd_t* f = &fds[fd];
    if (f->type != VFS_TYPE_DIR) return 0;

    int tar_count = tar_entry_count();
    int cursor = (int)f->offset;

    /* Phase 1: iterate TAR entries (indices 0..tar_count-1) */
    while (cursor < tar_count) {
        char entry_name[100];
        size_t entry_size = 0;
        if (!tar_get_entry(cursor, entry_name, &entry_size)) {
            break;
        }
        cursor++;
        f->offset = cursor;

        /* Skip directory entries (names ending with '/') */
        size_t len = 0;
        while (entry_name[len]) len++;
        if (len > 0 && entry_name[len - 1] == '/') continue;

        /* Copy entry name to caller's buffer */
        for (int i = 0; i < 99 && entry_name[i] != '\0'; i++) {
            name_buf[i] = entry_name[i];
        }
        name_buf[len < 99 ? len : 99] = '\0';
        *size_buf = entry_size;
        return 1;
    }

    /* Phase 2: iterate tmpfs entries */
    int tmpfs_cursor = cursor - tar_count;
    while (tmpfs_cursor < TMPFS_MAX_FILES) {
        if (tmpfs_get_entry(tmpfs_cursor, name_buf, size_buf)) {
            f->offset = cursor + 1;
            return 1;
        }
        cursor++;
        tmpfs_cursor++;
        f->offset = cursor;
    }

    return 0; /* No more entries */
}

/* ─── Write ────────────────────────────────────────────────────── */

int vfs_write(uint32_t pid, int fd, const void* buf, size_t count) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS || !buf) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    vfs_fd_t* f = &fds[fd];
    if (f->type != VFS_TYPE_FILE) return -1;
    if (!f->writable || f->tmpfs_idx < 0) return -1;  /* TAR files are read-only */

    int result = tmpfs_write(f->tmpfs_idx, f->offset, buf, count);
    if (result > 0) {
        f->offset += (size_t)result;
        /* Update cached size from tmpfs */
        char tmp_name[100];
        size_t tmp_size = 0;
        tmpfs_get_entry(f->tmpfs_idx, tmp_name, &tmp_size);
        f->size = tmp_size;
    }
    return result;
}

/* ─── Create ──────────────────────────────────────────────────── */

int vfs_create(uint32_t pid, const char* filename) {
    (void)pid;
    if (!filename) return -1;
    int idx = tmpfs_create(filename);
    return (idx >= 0) ? 0 : -1;
}

/* ─── Unlink ──────────────────────────────────────────────────── */

int vfs_unlink(uint32_t pid, const char* filename) {
    (void)pid;
    if (!filename) return -1;
    return tmpfs_unlink(filename);
}

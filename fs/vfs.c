/*
 * GenOS Virtual File System (VFS) Implementation
 *
 * Provides a POSIX-like FD interface on top of the read-only TAR ramdisk.
 * FD tables live inside struct task (per-process, no global table needed).
 */
#include "vfs.h"
#include "tar.h"
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
    serial_write_string("[OK] VFS layer initialized.\n");
}

/* ─── Open ─────────────────────────────────────────────────────── */

int vfs_open(uint32_t pid, const char* filename) {
    (void)pid;
    if (!filename) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    /* Special case: "/" opens a directory listing of all tar entries */
    if (filename[0] == '/' && filename[1] == '\0') {
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (fds[i].type == VFS_TYPE_NONE) {
                fds[i].type   = VFS_TYPE_DIR;
                fds[i].name[0] = '/';
                fds[i].name[1] = '\0';
                fds[i].data   = NULL;
                fds[i].size   = 0;
                fds[i].offset = 0;
                return i;
            }
        }
        return -1; /* No free FD slots */
    }

    /* Regular file: look up in TAR backend */
    size_t file_size = 0;
    char* file_data = tar_read_file(filename, &file_size);
    if (!file_data) return -1;

    /* Find first free FD slot */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (fds[i].type == VFS_TYPE_NONE) {
            fds[i].type = VFS_TYPE_FILE;
            /* Copy filename (truncate at 99 chars) */
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
    return -1; /* No free FD slots */
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

    /* Clamp read count to remaining bytes */
    size_t remaining = f->size - f->offset;
    if (count > remaining) count = remaining;

    /* Copy data from ramdisk to caller's buffer */
    uint8_t* src = f->data + f->offset;
    uint8_t* dst = (uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    f->offset += count;

    return (int)count;
}

/* ─── Close ────────────────────────────────────────────────────── */

int vfs_close(uint32_t pid, int fd) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return -1;

    if (fds[fd].type == VFS_TYPE_NONE) return -1;

    /* Mark slot as free */
    fds[fd].type = VFS_TYPE_NONE;
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

int vfs_readdir(uint32_t pid, int fd, char* name_buf, size_t* size_buf) {
    (void)pid;
    if (fd < 0 || fd >= VFS_MAX_FDS || !name_buf || !size_buf) return 0;

    vfs_fd_t* fds = task_get_current_fds();
    if (!fds) return 0;

    vfs_fd_t* f = &fds[fd];
    if (f->type != VFS_TYPE_DIR) return 0;

    /*
     * Use f->offset as an iteration cursor (integer index into TAR).
     * tar_get_entry() returns 1 if entry exists, 0 when past end.
     */
    char entry_name[100];
    size_t entry_size = 0;

    while (1) {
        int idx = (int)f->offset;
        if (!tar_get_entry(idx, entry_name, &entry_size)) {
            return 0; /* No more entries */
        }

        f->offset++; /* Advance cursor for next call */

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
}

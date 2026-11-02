#include "syscall_user.h"
#include "uaccess.h"
#include "preempt.h"
#include "../kernel/task.h"
#include "../fs/vfs.h"
#include <limits.h>

int sys_user_read(int fd, void* user_buf, size_t count) {
    PREEMPT_GUARD;
    char buffer[1024];
    if (count > INT_MAX || user_write_prepare(user_buf, count)) return -1;
    size_t done = 0;
    do {
        size_t chunk = count - done;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        vfs_fd_t* fds = task_get_current_fds();
        if (!fds || fd < 0 || fd >= VFS_MAX_FDS) return done ? (int)done : -1;
        size_t offset = fds[fd].offset;
        int n = vfs_read(get_current_pid(), fd, buffer, chunk);
        if (n < 0) return done ? (int)done : -1;
        if (copy_to_user((void*)((uint64_t)user_buf + done), buffer, (size_t)n)) {
            fds[fd].offset = offset;
            return done ? (int)done : -1;
        }
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    } while (done < count);
    return (int)done;
}

int sys_user_write(int fd, const void* user_buf, size_t count) {
    PREEMPT_GUARD;
    char buffer[1024];
    if (count > INT_MAX || !user_access_ok(user_buf, count, 0)) return -1;
    size_t done = 0;
    do {
        size_t chunk = count - done;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (copy_from_user(buffer, (const void*)((uint64_t)user_buf + done), chunk))
            return done ? (int)done : -1;
        int n = vfs_write(get_current_pid(), fd, buffer, chunk);
        if (n < 0) return done ? (int)done : -1;
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    } while (done < count);
    return (int)done;
}

int sys_user_readdir(int fd, char* user_name, size_t* user_size) {
    PREEMPT_GUARD;
    char name[100] = {0};
    size_t size = 0;
    if (user_write_prepare(user_name, sizeof(name)) ||
        user_write_prepare(user_size, sizeof(size))) return -1;
    vfs_fd_t* fds = task_get_current_fds();
    if (!fds || fd < 0 || fd >= VFS_MAX_FDS) return -1;
    size_t offset = fds[fd].offset;
    int result = vfs_readdir(get_current_pid(), fd, name, &size);
    if (result > 0 && (copy_to_user(user_name, name, sizeof(name)) ||
                      copy_to_user(user_size, &size, sizeof(size)))) {
        fds[fd].offset = offset;
        return -1;
    }
    return result;
}

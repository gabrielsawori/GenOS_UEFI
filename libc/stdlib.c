#include "stdlib.h"
#include "syscall.h"

void exit(int status) {
    syscall(2, (uint64_t)status, 0, 0);
    while(1);
}

void user_sleep(uint32_t milliseconds) {
    syscall(4, (uint64_t)milliseconds, 0, 0);
}

/*
 * read_file() - Baca file dari TAR ramdisk ke buffer user-space.
 *
 * Kernel akan mencari file di ramdisk dan menyalin isinya ke buffer.
 * Return: jumlah byte yang dibaca, atau 0 jika file tidak ditemukan.
 */
int read_file(const char* filename, char* buffer, int max_size) {
    return (int)syscall(8, (uint64_t)filename, (uint64_t)buffer, (uint64_t)max_size);
}

/*
 * exec() - Jalankan program ELF dari ramdisk.
 *
 * Kernel memuat ELF & membuat user task baru. Program baru berjalan
 * bersamaan (concurrent) dengan pemanggil. Untuk menunggu child selesai,
 * pakai wait_pid() di loop polling.
 *
 * Return: PID child (>0) jika berhasil, -1 jika file tidak ditemukan.
 */
int exec(const char* filename) {
    return (int)syscall(9, (uint64_t)filename, 0, 0);
}

/*
 * wait_pid() - Cek apakah task dengan PID tertentu sudah selesai.
 *
 * Non-blocking: hanya cek state lalu return. Pemanggil bertanggung jawab
 * memanggil user_sleep() di antara polling agar tidak menyita CPU.
 *
 * Return: 0 = masih hidup, 1 = TASK_DEAD / tidak ditemukan.
 */
int wait_pid(int pid) {
    return (int)syscall(11, (uint64_t)pid, 0, 0);
}

/* === Shared Memory IPC (syscalls 17-20) === */

int shm_create(int size) {
    return (int)syscall(17, (uint64_t)(uint32_t)size, 0, 0);
}

void* shm_attach(int shmid) {
    uint64_t addr = syscall(18, (uint64_t)(int64_t)shmid, 0, 0);
    return (void*)addr;
}

int shm_detach(void* addr) {
    return (int)syscall(19, (uint64_t)addr, 0, 0);
}

int shm_destroy(int shmid) {
    return (int)syscall(20, (uint64_t)(int64_t)shmid, 0, 0);
}

/* === Buffer Cache Statistics (syscall 21) === */

int cache_get_stats(cache_stats_t* out) {
    return (int)syscall(21, (uint64_t)out, 0, 0);
}

/* === Process Management (syscall 22) === */

int fork(void) {
    return (int)syscall(22, 0, 0, 0);
}
#include "stdlib.h"
#include "syscall.h"

int get_cpu_id(void) { return (int)syscall(50, 0, 0, 0); }
int read_input_event(struct genos_input_event* event) {
    return (int)syscall(51, (uint64_t)event, 0, 0);
}
int get_system_info(struct genos_system_info* info) {
    return (int)syscall(52, (uint64_t)info, 0, 0);
}

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

/* Wait until a PID exits. Kernel blocks this task and releases the mutation
 * lock while waiting; returns 1 for a dead or unknown PID. */
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

/* === Timer (syscall 33) === */

uint64_t get_ticks(void) {
    return syscall(33, 0, 0, 0);
}

/* === Power Management (syscall 45) === */

void power_shutdown(void) {
    syscall(45, 0, 0, 0);
    while (1);  /* Should never reach here */
}

void power_restart(void) {
    syscall(45, 1, 0, 0);
    while (1);
}

/* The kernel owns allocation metadata and serializes operations across threads.
 * Returned pages belong to this address space; no kernel heap pointers cross
 * the ABI. Allocation is page-granular and includes a trailing guard page. */
void* malloc(size_t size) {
    return (void*)syscall(54, size, 0, 0);
}

void free(void* ptr) {
    (void)syscall(55, (uint64_t)ptr, 0, 0);
}

void* realloc(void* ptr, size_t new_size) {
    return (void*)syscall(56, (uint64_t)ptr, new_size, 0);
}

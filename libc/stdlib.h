#pragma once
#include <stdint.h>

/* Akhiri aplikasi (syscall 2) */
void exit(int status);

/* Tidur selama N milidetik (syscall 4) */
void user_sleep(uint32_t milliseconds);

/* Baca file dari ramdisk ke buffer. Return: jumlah byte, 0=tidak ditemukan (syscall 8) */
int read_file(const char* filename, char* buffer, int max_size);

/*
 * Jalankan program ELF dari ramdisk (syscall 9).
 * Return: PID child (>0) jika berhasil, -1 jika file tidak ditemukan.
 * Gunakan wait_pid() untuk menunggu child selesai sebelum melanjutkan.
 */
int exec(const char* filename);

/*
 * Cek status task berdasarkan PID (syscall 11). Non-blocking.
 * Return: 0 = task masih hidup, 1 = task sudah selesai (DEAD) / tidak ditemukan.
 *
 * Polling pattern di Ring 3:
 *     int pid = exec("app.elf");
 *     while (pid > 0 && !wait_pid(pid)) user_sleep(50);
 */
int wait_pid(int pid);

/* === Shared Memory IPC (syscalls 17-20) === */

/* Buat shared memory segment; return shmid (>=0) atau -1 */
int shm_create(int size);

/* Pasang segment ke address space; return virtual address atau NULL */
void* shm_attach(int shmid);

/* Lepas shared memory; return 0 sukses / -1 error */
int shm_detach(void* addr);

/* Hancurkan segment (harus sudah di-detach semua); return 0 / -1 */
int shm_destroy(int shmid);

/* === Buffer Cache Statistics (syscall 21) === */

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint32_t used_blocks;
    uint32_t total_blocks;
} cache_stats_t;

/* Ambil statistik performa buffer cache; return 0 sukses / -1 error */
int cache_get_stats(cache_stats_t* out);

/* === Process Management (syscall 22) === */

/* Kloning proses saat ini; return PID child ke parent, 0 ke child, -1 error */
int fork(void);

/* === Timer (syscall 33) === */

/* Ambil timer tick count dari kernel; digunakan untuk clock */
uint64_t get_ticks(void);

/* === Power Management (syscall 45) === */

/* Matikan mesin (ACPI shutdown). Tidak pernah kembali. */
void power_shutdown(void);

/* Restart mesin (keyboard controller reset). Tidak pernah kembali. */
void power_restart(void);
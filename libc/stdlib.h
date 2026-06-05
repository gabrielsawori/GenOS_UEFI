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
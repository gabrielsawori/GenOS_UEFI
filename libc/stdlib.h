#pragma once
#include <stdint.h>

/* Akhiri aplikasi (syscall 2) */
void exit(int status);

/* Tidur selama N milidetik (syscall 4) */
void user_sleep(uint32_t milliseconds);

/* Baca file dari ramdisk ke buffer. Return: jumlah byte, 0=tidak ditemukan (syscall 8) */
int read_file(const char* filename, char* buffer, int max_size);

/* Jalankan program ELF dari ramdisk. Return: 0=ok, -1=gagal (syscall 9) */
int exec(const char* filename);
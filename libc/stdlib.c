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
 * Kernel akan memuat ELF dan membuat user task baru.
 * Program baru berjalan bersamaan (concurrent) dengan pemanggil.
 * Return: 0 = berhasil, -1 = file tidak ditemukan.
 */
int exec(const char* filename) {
    return (int)syscall(9, (uint64_t)filename, 0, 0);
}
#pragma once
#include <stdint.h>

/*
 * GenOS User-Space Standard I/O Library (stdio.h)
 *
 * Menyediakan fungsi input/output untuk aplikasi Ring-3.
 * Semua fungsi di sini berkomunikasi dengan kernel melalui syscall.
 */

/* Cetak teks ke layar pada posisi Y tertentu (syscall 1) */
void print(const char* text, int y_pos);

/*
 * Baca satu karakter dari keyboard (syscall 3).
 * Non-blocking: mengembalikan 0 jika tidak ada tombol yang ditekan.
 * Untuk menunggu input, gunakan loop: while ((c = read_key()) == 0) sleep(10);
 */
char read_key(void);
#pragma once
#include <stdint.h>

/*
 * GenOS User-Space Standard I/O Library
 * Semua fungsi berkomunikasi dengan kernel melalui syscall.
 */

/* Cetak teks ke layar pada posisi Y default (syscall 1, kompatibilitas) */
void print(const char* text, int y_pos);

/* Baca 1 karakter dari keyboard. Non-blocking, return 0 jika kosong (syscall 3) */
char read_key(void);

/* Bersihkan seluruh layar (syscall 5) */
void clear_screen(void);

/* Cetak teks pada posisi (x, y) dengan warna foreground (syscall 6) */
void print_at(const char* text, int x, int y, uint32_t color);

/* Gambar 1 karakter pada posisi (x, y) dengan warna foreground (syscall 7) */
void draw_char(char c, int x, int y, uint32_t color);
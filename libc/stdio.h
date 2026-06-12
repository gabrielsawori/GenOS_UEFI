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

/*
 * Isi area persegi (x,y,w,h) dengan satu warna (syscall 10).
 * Berguna untuk membersihkan baris terminal sebelum menulis ulang
 * agar teks lama tidak tumpang tindih dengan teks baru.
 */
void fill_rect(int x, int y, int w, int h, uint32_t color);

/* === VFS File Operations (syscalls 12-16) === */

/* Seek modes */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Baca file dari ramdisk langsung (syscall 8, legacy) */
int read_file(const char* filename, char* buffer, int max_size);

/* Buka file atau direktori melalui VFS; return FD (>=0) atau -1 */
int open(const char* filename, int flags);

/* Baca hingga count byte dari FD ke buf; return bytes read / -1 */
int read(int fd, void* buf, int count);

/* Tutup file descriptor; return 0 sukses / -1 error */
int close(int fd);

/* Ubah posisi baca file; return posisi baru / -1 */
int seek(int fd, int offset, int whence);

/* Baca entry direktori berikutnya; return 1=found, 0=done */
int readdir(int fd, char* name_buf, int* size_buf);
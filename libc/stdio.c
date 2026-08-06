#include "stdio.h"
#include "syscall.h"

void print(const char* text, int y_pos) {
    syscall(1, (uint64_t)text, (uint64_t)y_pos, 0);
}

char read_key(void) {
    return (char)syscall(3, 0, 0, 0);
}

void clear_screen(void) {
    syscall(5, 0, 0, 0);
}

/*
 * print_at() - Cetak teks pada posisi (x, y) dengan warna foreground.
 *
 * Koordinat x dan y di-pack ke satu argumen 64-bit:
 *   arg2 = ((uint64_t)x << 32) | (uint32_t)y
 * Ini karena konvensi syscall GenOS hanya mendukung 3 argumen user.
 */
void print_at(const char* text, int x, int y, uint32_t color) {
    uint64_t packed_xy = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    syscall(6, (uint64_t)text, packed_xy, (uint64_t)color);
}

/*
 * draw_char() - Gambar 1 karakter pada posisi (x, y).
 * Menggunakan packing koordinat yang sama dengan print_at().
 */
void draw_char(char c, int x, int y, uint32_t color) {
    uint64_t packed_xy = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    syscall(7, (uint64_t)c, packed_xy, (uint64_t)color);
}

/*
 * fill_rect() - Isi blok persegi (x,y,w,h) dengan satu warna.
 *
 * Pack koordinat & ukuran ke 2 argumen 64-bit karena syscall hanya
 * mendukung 3 argumen user:
 *   arg1 = (x << 32) | y
 *   arg2 = (w << 32) | h
 *   arg3 = color
 */
void fill_rect(int x, int y, int w, int h, uint32_t color) {
    uint64_t packed_xy = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    uint64_t packed_wh = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    syscall(10, packed_xy, packed_wh, (uint64_t)color);
}

/* === VFS File Operations (syscalls 12-16) === */

/*
 * open() - Buka file atau direktori melalui VFS (syscall 12).
 * filename = "/" membuka listing direktori.
 * Return: FD number (>=0) atau -1 bila gagal.
 */
int open(const char* filename, int flags) {
    return (int)syscall(12, (uint64_t)filename, (uint64_t)flags, 0);
}

/*
 * read() - Baca dari file descriptor (syscall 13).
 * Return: jumlah byte yang dibaca, 0 = EOF, -1 = error.
 */
int read(int fd, void* buf, int count) {
    return (int)syscall(13, (uint64_t)(int64_t)fd, (uint64_t)buf, (uint64_t)count);
}

/*
 * close() - Tutup file descriptor (syscall 14).
 * Return: 0 = sukses, -1 = error.
 */
int close(int fd) {
    return (int)syscall(14, (uint64_t)(int64_t)fd, 0, 0);
}

/*
 * seek() - Ubah posisi baca file (syscall 15).
 * whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
 * Return: posisi baru atau -1 bila error.
 */
int seek(int fd, int offset, int whence) {
    return (int)syscall(15, (uint64_t)(int64_t)fd, (uint64_t)(int64_t)offset, (uint64_t)whence);
}

/*
 * readdir() - Baca entry direktori berikutnya (syscall 16).
 * FD harus dibuka dengan open("/", 0).
 * name_buf diisi nama file, *size_buf diisi ukuran.
 * Return: 1 = entry ditemukan, 0 = tidak ada entry lagi.
 */
int readdir(int fd, char* name_buf, int* size_buf) {
    /* size_buf is int* but kernel expects size_t* — cast through uint64_t */
    return (int)syscall(16, (uint64_t)(int64_t)fd, (uint64_t)name_buf, (uint64_t)size_buf);
}

/* === Write Operations (syscalls 23-25) === */

int write(int fd, const void* buf, int count) {
    return (int)syscall(23, (uint64_t)(int64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int create(const char* filename) {
    return (int)syscall(24, (uint64_t)filename, 0, 0);
}

int unlink(const char* filename) {
    return (int)syscall(25, (uint64_t)filename, 0, 0);
}

/* === Mouse Input (syscall 30) === */

/*
 * read_mouse() - Ambil snapshot state mouse dari kernel.
 * Kernel menyalin (x, y, buttons, changed) ke buffer user dan mereset
 * flag 'changed' (event dikonsumsi).
 */
int read_mouse(mouse_state_t* m) {
    return (int)syscall(30, (uint64_t)m, 0, 0);
}
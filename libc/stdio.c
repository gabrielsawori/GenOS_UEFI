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
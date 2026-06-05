#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../limine.h"

// Deklarasi fungsi-fungsi layar kita
void fb_init(struct limine_framebuffer *framebuffer);
void fb_draw_pixel(size_t x, size_t y, uint32_t color);
void fb_draw_char(char c, size_t x, size_t y, uint32_t fg_color, uint32_t bg_color, int scale);
void fb_print(const char *str, size_t x, size_t y, uint32_t fg_color, uint32_t bg_color, int scale);
/*
 * fb_fill_rect() - Isi area persegi pada framebuffer dengan satu warna.
 *
 * Digunakan untuk membersihkan baris terminal sebelum menulis ulang teks
 * sehingga karakter lama tidak tumpang tindih dengan karakter baru
 * (mis. saat shell mencetak hasil perintah berulang di posisi yang sama).
 *
 * Parameter di-clamp aman terhadap batas layar di dalam fb_draw_pixel().
 */
void fb_fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color);
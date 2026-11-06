/*
 * font_renderer.h — API publik font TrueType untuk kernel GenOS.
 *
 * Semua parameter/return bertipe integer (tidak ada float) agar bisa
 * dipanggil dari kode kernel yang di-compile tanpa SSE.
 *
 * Menggunakan stb_truetype.h secara internal untuk parsing dan
 * rasterisasi font TrueType. Glyph di-cache untuk performa.
 *
 * Penggunaan:
 *   1. Panggil ttf_init() dengan data font .ttf dari ramdisk
 *   2. Gunakan ttf_print() / ttf_draw_char() untuk render teks
 *   3. Cek ttf_is_initialized() untuk fallback ke bitmap font
 */
#ifndef FONT_RENDERER_H
#define FONT_RENDERER_H

#include <stdint.h>
#include <stddef.h>

/*
 * ttf_init — Inisialisasi font renderer dari data TTF.
 *
 * @font_data:    Pointer ke raw data file .ttf (dari ramdisk)
 * @font_size:    Ukuran data dalam byte
 * @pixel_height: Tinggi huruf dalam pixel (misalnya 16, 20, 24)
 *
 * Return: 1 jika berhasil, 0 jika gagal.
 *
 * PENTING: Harus dipanggil SETELAH heap_init() (butuh kmalloc).
 */
int ttf_init(const uint8_t *font_data, size_t font_size, int pixel_height);

/*
 * ttf_draw_char — Render 1 karakter TrueType ke framebuffer.
 *
 * @c:        Karakter ASCII yang akan digambar
 * @x, @y:    Posisi kiri-atas pada layar (pixel)
 * @fg_color: Warna foreground (0xRRGGBB)
 *
 * Karakter di-render dengan anti-aliasing (alpha blending).
 */
void ttf_draw_char(char c, int x, int y, uint32_t fg_color);

/*
 * ttf_print — Render string ke framebuffer dengan TrueType.
 *
 * @str:      String null-terminated yang akan digambar
 * @x, @y:    Posisi awal (kiri-atas) pada layar
 * @fg_color: Warna foreground (0xRRGGBB)
 *
 * Mendukung newline (\n) dan auto-kerning antar huruf.
 * Return: Posisi X terakhir setelah string selesai di-render.
 */
int ttf_print(const char *str, int x, int y, uint32_t fg_color);

/*
 * ttf_get_char_width — Dapatkan lebar karakter dalam pixel.
 *
 * Return: Lebar advance karakter (jarak ke karakter berikutnya).
 */
int ttf_get_char_width(char c);

/*
 * ttf_get_line_height — Dapatkan tinggi baris dalam pixel.
 *
 * Termasuk ascent + descent + line gap.
 */
int ttf_get_line_height(void);

/*
 * ttf_is_initialized — Cek apakah font TrueType sudah aktif.
 *
 * Return: 1 jika TTF ready, 0 jika belum (gunakan bitmap fallback).
 */
int ttf_is_initialized(void);

#endif /* FONT_RENDERER_H */

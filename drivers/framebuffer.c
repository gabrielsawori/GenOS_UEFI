#include "framebuffer.h"
#include "font8x8_basic.h" // Memasukkan array peta huruf yang baru kita unduh

static struct limine_framebuffer *fb = NULL;
static uint32_t *fb_ptr = NULL;

void fb_init(struct limine_framebuffer *framebuffer) {
    fb = framebuffer;
    fb_ptr = (uint32_t *)fb->address;
}

// Fungsi paling dasar: Mewarnai 1 titik piksel
void fb_draw_pixel(size_t x, size_t y, uint32_t color) {
    // Jangan menggambar di luar batas layar agar tidak merusak memori lain
    if (!fb || x >= fb->width || y >= fb->height) return;
    
    size_t pixel_index = (y * (fb->pitch / 4)) + x;
    fb_ptr[pixel_index] = color;
}

// Fungsi menggambar 1 huruf
// FIX BUG #1: Gunakan cast (unsigned char) untuk keamanan
void fb_draw_char(char c, size_t x, size_t y, uint32_t fg, uint32_t bg, int scale) {
    if ((unsigned char)c > 127) return; // Hanya dukung ASCII dasar (0-127) - FIXED
    
    // Ambil pola biner untuk huruf tersebut dari font8x8
    unsigned char *glyph = (unsigned char *)font8x8_basic[(int)c];

    // Cek setiap baris (8 baris) dan kolom (8 kolom) dari matriks huruf
    for (int cy = 0; cy < 8; cy++) {
        for (int cx = 0; cx < 8; cx++) {
            // Apakah di titik ini ada tinta (bit bernilai 1)?
            uint32_t color = (glyph[cy] & (1 << cx)) ? fg : bg;
            
            // Perbesar huruf sesuai variabel 'scale'
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fb_draw_pixel(x + (cx * scale) + sx, y + (cy * scale) + sy, color);
                }
            }
        }
    }
}

// Fungsi menggambar kalimat panjang
void fb_print(const char *str, size_t x, size_t y, uint32_t fg, uint32_t bg, int scale) {
    size_t curr_x = x;
    size_t curr_y = y;
    
    for (int i = 0; str[i] != '\0'; i++) {
        // Jika bertemu Enter / Baris Baru
        if (str[i] == '\n') {
            curr_x = x;
            curr_y += 8 * scale;
            continue;
        }
        
        // Cetak huruf saat ini
        fb_draw_char(str[i], curr_x, curr_y, fg, bg, scale);
        
        // Geser posisi kursor ke kanan untuk huruf berikutnya
        curr_x += 8 * scale;
        
        // Jika mentok di ujung kanan layar, otomatis turun ke baris bawah
        if (curr_x + (8 * scale) > fb->width) {
            curr_x = x;
            curr_y += 8 * scale;
        }
    }
}

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

/*
 * fb_fill_rect() - Isi blok persegi (x,y,w,h) dengan satu warna.
 *
 * Dipakai shell untuk membersihkan baris sebelum mencetak teks baru,
 * sehingga karakter lama tidak tumpang tindih dengan karakter baru.
 * fb_draw_pixel() sudah meng-handle clipping di luar batas layar.
 */
void fb_fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color) {
    for (size_t dy = 0; dy < h; dy++) {
        for (size_t dx = 0; dx < w; dx++) {
            fb_draw_pixel(x + dx, y + dy, color);
        }
    }
}

uint32_t fb_read_pixel(size_t x, size_t y) {
    if (!fb || x >= fb->width || y >= fb->height) return 0;
    size_t pixel_index = (y * (fb->pitch / 4)) + x;
    return fb_ptr[pixel_index];
}

size_t fb_get_width(void)  { return fb ? fb->width  : 0; }
size_t fb_get_height(void) { return fb ? fb->height : 0; }

/* ==================================================================
 * MOUSE CURSOR OVERLAY
 * ==================================================================
 *
 * Classic arrow pointer, 12 wide x 17 tall.
 * Encoded as: 0 = transparent, 1 = black (outline), 2 = white (fill)
 *
 * The cursor is rendered directly to the framebuffer with automatic
 * background save/restore for flicker-free movement.
 */

#define CURSOR_W 12
#define CURSOR_H 17

/* Arrow cursor bitmap — each row is 12 pixels */
static const uint8_t cursor_bitmap[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
};

/* Background buffer: saves pixels under cursor before drawing */
static uint32_t cursor_bg[CURSOR_H][CURSOR_W];
static int32_t  cursor_x = -1, cursor_y = -1;
static int      cursor_visible = 0;

/* Save background pixels at (cx, cy) */
static void cursor_save_bg(int32_t cx, int32_t cy) {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t px = cx + x;
            int32_t py = cy + y;
            if (px >= 0 && (size_t)px < fb->width &&
                py >= 0 && (size_t)py < fb->height) {
                cursor_bg[y][x] = fb_read_pixel(px, py);
            }
        }
    }
}

/* Restore background pixels at (cx, cy) */
static void cursor_restore_bg(int32_t cx, int32_t cy) {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t px = cx + x;
            int32_t py = cy + y;
            if (px >= 0 && (size_t)px < fb->width &&
                py >= 0 && (size_t)py < fb->height) {
                fb_draw_pixel(px, py, cursor_bg[y][x]);
            }
        }
    }
}

/* Draw cursor sprite at (cx, cy) */
static void cursor_draw_at(int32_t cx, int32_t cy) {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            uint8_t pixel = cursor_bitmap[y][x];
            if (pixel == 0) continue; /* Transparent */

            int32_t px = cx + x;
            int32_t py = cy + y;
            if (px >= 0 && (size_t)px < fb->width &&
                py >= 0 && (size_t)py < fb->height) {
                uint32_t color = (pixel == 1) ? 0x000000 : 0xFFFFFF;
                fb_draw_pixel(px, py, color);
            }
        }
    }
}

/*
 * cursor_update() — Move cursor to new position.
 * Called from mouse IRQ handler for instant response.
 */
void cursor_update(int32_t x, int32_t y) {
    if (!fb) return;

    /* If cursor hasn't moved, skip */
    if (x == cursor_x && y == cursor_y && cursor_visible) return;

    /* 1. Restore old background (erase cursor at old position) */
    if (cursor_visible) {
        cursor_restore_bg(cursor_x, cursor_y);
    }

    /* 2. Update position */
    cursor_x = x;
    cursor_y = y;

    /* 3. Save new background */
    cursor_save_bg(cursor_x, cursor_y);

    /* 4. Draw cursor at new position */
    cursor_draw_at(cursor_x, cursor_y);
    cursor_visible = 1;
}

void cursor_hide(void) {
    if (!fb || !cursor_visible) return;
    cursor_restore_bg(cursor_x, cursor_y);
    cursor_visible = 0;
}

void cursor_show(void) {
    if (!fb || cursor_visible) return;
    cursor_save_bg(cursor_x, cursor_y);
    cursor_draw_at(cursor_x, cursor_y);
    cursor_visible = 1;
}

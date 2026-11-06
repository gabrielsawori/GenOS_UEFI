/*
 * font_renderer.c — Implementasi TrueType font renderer untuk GenOS.
 *
 * File ini HARUS di-compile dengan SSE2 (-msse2 -mfpmath=sse)
 * karena stb_truetype menggunakan operasi float secara internal.
 *
 * Semua dependensi C library (math, malloc, string) di-override
 * ke implementasi kernel (soft_math, kmalloc, memcpy/memset).
 *
 * Glyph ASCII (32-126) di-cache saat init untuk performa rendering.
 */

#include <stdint.h>
#include <stddef.h>
#include "soft_math.h"
#include "../framebuffer.h"
#include "../../mm/heap.h"
#include "../../kernel/utils.h"
#include "../../drivers/serial.h"

/* ══════════════════════════════════════════════════════════════════════
 * Override semua dependensi C library sebelum include stb_truetype.
 * Ini mencegah stb_truetype dari #include <math.h>, <stdlib.h>, dll.
 * ══════════════════════════════════════════════════════════════════════ */

/* --- Math (soft_math.h) --- */
#define STBTT_ifloor(x)    ((int) soft_floor(x))
#define STBTT_iceil(x)     ((int) soft_ceil(x))
#define STBTT_sqrt(x)      soft_sqrt(x)
#define STBTT_pow(x,y)     soft_pow(x,y)
#define STBTT_fmod(x,y)    soft_fmod(x,y)
#define STBTT_cos(x)       soft_cos(x)
#define STBTT_acos(x)      soft_acos(x)
#define STBTT_fabs(x)      soft_fabs(x)

/* --- Memory Allocation (kernel heap) --- */
#define STBTT_malloc(x,u)  ((void)(u), kmalloc(x))
#define STBTT_free(x,u)    ((void)(u), kfree(x))

/* --- Assert (disabled in kernel — no abort) --- */
#define STBTT_assert(x)    ((void)(x))

/* --- String Functions (kernel utils) --- */
static inline size_t _ttf_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}
#define STBTT_strlen(x)    _ttf_strlen(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset

/* ══════════════════════════════════════════════════════════════════════
 * Include stb_truetype implementation
 * ══════════════════════════════════════════════════════════════════════ */
#define STB_TRUETYPE_IMPLEMENTATION
#include "font_modern/stb_truetype.h"

/* ══════════════════════════════════════════════════════════════════════
 * Glyph Cache — Pre-render semua karakter ASCII yang bisa dicetak
 * ══════════════════════════════════════════════════════════════════════ */

#define CACHE_FIRST  32   /* Spasi */
#define CACHE_LAST   126  /* Tilde ~ */
#define CACHE_COUNT  (CACHE_LAST - CACHE_FIRST + 1) /* 95 karakter */

typedef struct {
    uint8_t *bitmap;         /* Pixel bitmap (alpha/coverage 0-255) */
    int      w, h;           /* Ukuran bitmap dalam pixel */
    int      xoff, yoff;     /* Offset relatif ke baseline */
    int      advance_width;  /* Jarak ke karakter berikutnya (unscaled) */
} cached_glyph_t;

static stbtt_fontinfo  font_info;
static cached_glyph_t  glyph_cache[CACHE_COUNT];
static int             font_initialized = 0;
static int             current_pixel_height = 16;
static float           current_scale = 0.0f;
static int             font_ascent, font_descent, font_line_gap;

/* ══════════════════════════════════════════════════════════════════════
 * Alpha Blending — Campurkan warna teks + latar berdasarkan coverage
 * ══════════════════════════════════════════════════════════════════════ */
static inline uint32_t alpha_blend(uint8_t coverage, uint32_t fg, uint32_t bg) {
    if (coverage == 255) return fg;
    if (coverage == 0)   return bg;

    uint32_t a = coverage;
    uint32_t ia = 255 - a;

    uint32_t fr = (fg >> 16) & 0xFF;
    uint32_t fg_g = (fg >> 8) & 0xFF;
    uint32_t fb_b = fg & 0xFF;

    uint32_t br = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bb = bg & 0xFF;

    uint32_t r = (fr * a + br * ia) / 255;
    uint32_t g = (fg_g * a + bg_g * ia) / 255;
    uint32_t b = (fb_b * a + bb * ia) / 255;

    return (r << 16) | (g << 8) | b;
}

/* ══════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════════════ */

int ttf_init(const uint8_t *font_data, size_t font_size, int pixel_height) {
    (void)font_size;  /* stb_truetype tidak perlu ukuran */

    if (!font_data) {
        serial_write_string("[TTF] ERROR: font_data is NULL\n");
        return 0;
    }

    /* Cari offset font pertama dalam file TTC/TTF */
    int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
    if (offset < 0) {
        serial_write_string("[TTF] ERROR: invalid font offset\n");
        return 0;
    }

    /* Parse header font */
    if (!stbtt_InitFont(&font_info, font_data, offset)) {
        serial_write_string("[TTF] ERROR: stbtt_InitFont failed\n");
        return 0;
    }

    current_pixel_height = pixel_height;
    current_scale = stbtt_ScaleForPixelHeight(&font_info, (float)pixel_height);

    /* Ambil metrik vertikal font */
    stbtt_GetFontVMetrics(&font_info, &font_ascent, &font_descent, &font_line_gap);

    serial_write_string("[TTF] Pre-caching ASCII glyphs...\n");

    /* Pre-cache semua karakter ASCII yang bisa dicetak */
    for (int c = CACHE_FIRST; c <= CACHE_LAST; c++) {
        int idx = c - CACHE_FIRST;
        int advance, lsb;

        stbtt_GetCodepointHMetrics(&font_info, c, &advance, &lsb);
        glyph_cache[idx].advance_width = advance;

        /* Render glyph ke bitmap monokrom (alpha 0-255) */
        glyph_cache[idx].bitmap = stbtt_GetCodepointBitmap(
            &font_info,
            0,                  /* scale_x (gunakan scale_y saja) */
            current_scale,      /* scale_y */
            c,                  /* codepoint */
            &glyph_cache[idx].w,
            &glyph_cache[idx].h,
            &glyph_cache[idx].xoff,
            &glyph_cache[idx].yoff
        );
    }

    font_initialized = 1;
    serial_write_string("[TTF] Font renderer initialized OK\n");
    return 1;
}

void ttf_draw_char(char c, int x, int y, uint32_t fg_color) {
    if (!font_initialized) return;
    if ((unsigned char)c < CACHE_FIRST || (unsigned char)c > CACHE_LAST) return;

    int idx = (unsigned char)c - CACHE_FIRST;
    cached_glyph_t *g = &glyph_cache[idx];
    if (!g->bitmap || g->w == 0 || g->h == 0) return;

    /* Hitung posisi baseline (ascent di-scale) */
    int baseline_y = y + (int)(font_ascent * current_scale);

    /* Blit bitmap glyph ke framebuffer dengan alpha blending */
    for (int gy = 0; gy < g->h; gy++) {
        for (int gx = 0; gx < g->w; gx++) {
            uint8_t coverage = g->bitmap[gy * g->w + gx];
            if (coverage == 0) continue;  /* Transparan — skip */

            int px = x + gx + g->xoff;
            int py = baseline_y + gy + g->yoff;

            if (px < 0 || py < 0) continue;

            if (coverage == 255) {
                /* Fully opaque — langsung tulis */
                fb_draw_pixel((size_t)px, (size_t)py, fg_color);
            } else {
                /* Semi-transparan — blend dengan pixel latar */
                uint32_t bg = fb_read_pixel((size_t)px, (size_t)py);
                fb_draw_pixel((size_t)px, (size_t)py,
                              alpha_blend(coverage, fg_color, bg));
            }
        }
    }
}

int ttf_get_char_width(char c) {
    if (!font_initialized) return 8;  /* Fallback lebar bitmap 8x8 */
    if ((unsigned char)c < CACHE_FIRST || (unsigned char)c > CACHE_LAST) return 0;

    int idx = (unsigned char)c - CACHE_FIRST;
    return (int)(glyph_cache[idx].advance_width * current_scale);
}

int ttf_get_line_height(void) {
    if (!font_initialized) return 16; /* Fallback 8x8 × scale 2 */
    return (int)((font_ascent - font_descent + font_line_gap) * current_scale);
}

int ttf_print(const char *str, int x, int y, uint32_t fg_color) {
    if (!font_initialized) return x;

    int curr_x = x;
    for (int i = 0; str[i] != '\0'; i++) {
        /* Handle newline */
        if (str[i] == '\n') {
            curr_x = x;
            y += ttf_get_line_height();
            continue;
        }

        /* Render karakter */
        ttf_draw_char(str[i], curr_x, y, fg_color);
        curr_x += ttf_get_char_width(str[i]);

        /* Kerning: penyesuaian jarak antar pasangan huruf */
        if (str[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(
                &font_info, str[i], str[i + 1]);
            curr_x += (int)(kern * current_scale);
        }

        /* Wrap otomatis jika mentok kanan layar */
        size_t screen_w = fb_get_width();
        if (screen_w > 0 && (size_t)curr_x + 20 > screen_w) {
            curr_x = x;
            y += ttf_get_line_height();
        }
    }
    return curr_x;
}

int ttf_is_initialized(void) {
    return font_initialized;
}

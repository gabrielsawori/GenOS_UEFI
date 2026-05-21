#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../limine.h"

// Deklarasi fungsi-fungsi layar kita
void fb_init(struct limine_framebuffer *framebuffer);
void fb_draw_pixel(size_t x, size_t y, uint32_t color);
void fb_draw_char(char c, size_t x, size_t y, uint32_t fg_color, uint32_t bg_color, int scale);
void fb_print(const char *str, size_t x, size_t y, uint32_t fg_color, uint32_t bg_color, int scale);
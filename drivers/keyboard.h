#pragma once
#include <stdint.h>

void keyboard_handler(uint8_t scancode);

// Fungsi baru untuk Program Shell mengambil huruf dari antrean
char keyboard_get_char(void);
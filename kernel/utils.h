#include <stddef.h>
#pragma once
#include <stdint.h>

// Mengubah angka menjadi teks
void itoa(uint64_t num, char* str, int base);

// Membandingkan dua kata (String Compare)
int strcmp(const char *s1, const char *s2);
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);

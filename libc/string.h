#pragma once
#include <stddef.h>

/* Guard: prevent system <string.h> from being included */
#define _STRING_H

/* Bandingkan dua string. Return: 0=sama, <0 atau >0 jika berbeda */
int strcmp(const char* s1, const char* s2);

/* Hitung panjang string */
size_t strlen(const char* s);

/* Copy n bytes dari src ke dest (tidak boleh overlap) */
void* memcpy(void* dest, const void* src, size_t n);

/* Set n bytes di dest ke nilai c */
void* memset(void* dest, int c, size_t n);

/* Bandingkan n bytes. Return: 0=sama */
int memcmp(const void* s1, const void* s2, size_t n);

/* Copy n bytes (aman untuk overlap) */
void* memmove(void* dest, const void* src, size_t n);

/* Absolute value */
int abs(int x);

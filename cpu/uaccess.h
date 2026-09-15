#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * User-Space Access Validation — cpu/uaccess.h
 *
 * Modul ini mencegah serangan kernel memory read/write dari Ring 3.
 * Tanpa validasi ini, user bisa mengirim pointer ke alamat kernel
 * (mis. 0xFFFF800000000000) melalui syscall dan membaca/menulis
 * memori kernel secara arbitrary.
 *
 * Aturan x86-64 canonical address:
 *   User space:  0x0000000000000000 — 0x00007FFFFFFFFFFF
 *   Kernel space: 0xFFFF800000000000 — 0xFFFFFFFFFFFFFFFF
 *   Non-canonical (hole): 0x0000800000000000 — 0xFFFF7FFFFFFFFFFF
 */

/* Batas atas alamat user-space canonical (x86-64) */
#define USER_ADDR_MAX  0x800000000000ULL

/*
 * is_user_range() — Validasi bahwa range [addr, addr+len) sepenuhnya
 * berada di user-space. Mencegah:
 *   - Pointer ke kernel space (HHDM, page tables, dll)
 *   - Pointer ke non-canonical hole
 *   - Integer overflow (addr + len wrapping)
 *
 * Return: 1 jika valid, 0 jika berbahaya.
 */
static inline int is_user_range(uint64_t addr, size_t len) {
    if (len == 0) return 1;
    if (addr >= USER_ADDR_MAX) return 0;
    if (addr + len > USER_ADDR_MAX) return 0;
    if (addr + len < addr) return 0;  /* overflow check */
    return 1;
}

/*
 * verify_user_ptr() — Verifikasi satu pointer user-space.
 * Shorthand untuk is_user_range dengan len minimal (1 byte).
 * Return: 1 jika valid, 0 jika berbahaya.
 */
static inline int verify_user_ptr(const void* ptr) {
    return (uint64_t)ptr < USER_ADDR_MAX;
}

/*
 * verify_user_string() — Verifikasi string user-space (null-terminated).
 * Scan hingga max_len byte, pastikan setiap byte masih di user-space.
 * Return: panjang string jika valid, -1 jika pointer berbahaya atau
 *         string melebihi max_len tanpa null terminator.
 */
static inline int verify_user_string(const char* str, size_t max_len) {
    if (!verify_user_ptr(str)) return -1;
    for (size_t i = 0; i < max_len; i++) {
        if ((uint64_t)(str + i) >= USER_ADDR_MAX) return -1;
        if (str[i] == '\0') return (int)i;
    }
    return -1;  /* string terlalu panjang */
}

/*
 * copy_from_user() — Salin data dari user-space ke kernel secara aman.
 * Validasi range sebelum menyalin. Jika SMAP aktif, gunakan STAC/CLAC.
 *
 * Return: 0 sukses, -1 jika pointer invalid.
 */
static inline int copy_from_user(void* kernel_dst, const void* user_src, size_t len) {
    if (!is_user_range((uint64_t)user_src, len)) return -1;
    const uint8_t* s = (const uint8_t*)user_src;
    uint8_t* d = (uint8_t*)kernel_dst;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

/*
 * copy_to_user() — Salin data dari kernel ke user-space secara aman.
 * Validasi range sebelum menyalin.
 *
 * Return: 0 sukses, -1 jika pointer invalid.
 */
static inline int copy_to_user(void* user_dst, const void* kernel_src, size_t len) {
    if (!is_user_range((uint64_t)user_dst, len)) return -1;
    const uint8_t* s = (const uint8_t*)kernel_src;
    uint8_t* d = (uint8_t*)user_dst;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

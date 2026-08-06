#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * CSPRNG — Cryptographically Secure Pseudo-Random Number Generator.
 *
 * Arsitektur:
 *   1. Seed dari RDRAND/RDSEED (hardware entropy, x86-64)
 *   2. Fallback ke TSC (Time Stamp Counter) + jitter mixing jika
 *      RDRAND tidak tersedia
 *   3. Output generation via ChaCha20 stream cipher
 *   4. Automatic reseed setiap RESEED_INTERVAL byte
 *
 * Thread-safe: menggunakan spinlock internal.
 *
 * PENTING: csprng_init() HARUS dipanggil setelah heap_init()
 *          dan sebelum subsistem lain yang membutuhkan random.
 */

/* Inisialisasi CSPRNG. Harus dipanggil satu kali saat boot. */
void csprng_init(void);

/* Isi buffer dengan random bytes berkualitas kriptografis */
void csprng_get_bytes(uint8_t *buf, size_t len);

/* Dapatkan satu bilangan random 64-bit */
uint64_t csprng_get_u64(void);

/* Dapatkan satu bilangan random 32-bit */
uint32_t csprng_get_u32(void);

/*
 * Tambahkan entropy tambahan ke pool (misalnya dari interrupt timing,
 * keyboard input, mouse movement, dll).
 */
void csprng_add_entropy(const uint8_t *data, size_t len);

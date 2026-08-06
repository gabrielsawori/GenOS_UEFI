#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 — FIPS 180-4 Secure Hash Algorithm.
 *
 * Menghasilkan digest 256-bit (32 byte) dari data input berapa pun panjangnya.
 * Digunakan untuk: integrity check, HMAC, password hashing, CSPRNG seeding.
 *
 * Penggunaan:
 *   sha256_ctx_t ctx;
 *   sha256_init(&ctx);
 *   sha256_update(&ctx, data, len);
 *   sha256_final(&ctx, hash);   // hash = uint8_t[32]
 *
 * Atau untuk data sekali jalan:
 *   sha256_hash(data, len, hash);
 */

#define SHA256_BLOCK_SIZE  64   /* 512 bits */
#define SHA256_DIGEST_SIZE 32   /* 256 bits */

typedef struct {
    uint32_t state[8];          /* H0..H7 */
    uint64_t total_len;         /* Total byte yang sudah di-update */
    uint8_t  buffer[SHA256_BLOCK_SIZE]; /* Buffer partial block */
    uint32_t buf_len;           /* Panjang data di buffer */
} sha256_ctx_t;

/* Inisialisasi konteks SHA-256 dengan Initial Hash Values */
void sha256_init(sha256_ctx_t *ctx);

/* Tambahkan data ke hash. Bisa dipanggil berkali-kali (streaming) */
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/* Finalisasi: padding + hasilkan digest 32 byte */
void sha256_final(sha256_ctx_t *ctx, uint8_t *hash);

/* Convenience: hash seluruh data sekaligus */
void sha256_hash(const uint8_t *data, size_t len, uint8_t *hash);

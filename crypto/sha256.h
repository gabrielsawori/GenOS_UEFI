#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 Cryptographic Hash Function
 *
 * FIPS 180-4 compliant implementation.
 * Produces a 256-bit (32-byte) message digest.
 *
 * Usage:
 *   sha256_ctx_t ctx;
 *   sha256_init(&ctx);
 *   sha256_update(&ctx, data, len);
 *   sha256_final(&ctx, hash);  // hash must be uint8_t[32]
 *
 * Or one-shot:
 *   uint8_t hash[32];
 *   sha256_hash(data, len, hash);
 */

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];       /* H0..H7 intermediate hash */
    uint64_t bitcount;       /* Total bits processed */
    uint8_t  buffer[64];     /* Partial block buffer */
    uint32_t buflen;         /* Bytes in buffer */
} sha256_ctx_t;

/* Initialize context for new hash computation */
void sha256_init(sha256_ctx_t* ctx);

/* Feed data into hash computation (can be called multiple times) */
void sha256_update(sha256_ctx_t* ctx, const void* data, size_t len);

/* Finalize and output 32-byte digest */
void sha256_final(sha256_ctx_t* ctx, uint8_t digest[32]);

/* One-shot convenience: hash data into 32-byte digest */
void sha256_hash(const void* data, size_t len, uint8_t digest[32]);

/* Convert 32-byte digest to 64-char hex string (+ NUL) */
void sha256_to_hex(const uint8_t digest[32], char hex[65]);

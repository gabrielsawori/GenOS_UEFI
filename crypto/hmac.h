#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * HMAC-SHA256 — Keyed-Hash Message Authentication Code
 *
 * RFC 2104 / FIPS 198-1 compliant.
 * Produces a 256-bit (32-byte) authentication tag.
 *
 * Usage:
 *   uint8_t mac[32];
 *   hmac_sha256(key, key_len, message, msg_len, mac);
 */

#define HMAC_SHA256_SIZE 32

/* Compute HMAC-SHA256(key, message) → 32-byte mac */
void hmac_sha256(const void* key, size_t key_len,
                 const void* data, size_t data_len,
                 uint8_t mac[32]);

/*
 * PBKDF2-HMAC-SHA256 — Password-Based Key Derivation Function 2
 *
 * RFC 8018 compliant. Stretches a password into a derived key.
 * Used for password hashing with configurable iteration count.
 *
 * iterations: recommended minimum 10000 for passwords
 * dk_len: desired output key length in bytes (max 32 for single block)
 */
void pbkdf2_hmac_sha256(const void* password, size_t pass_len,
                        const void* salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t* dk, size_t dk_len);

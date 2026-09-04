#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * AES-256-CBC Encryption / Decryption
 *
 * FIPS 197 (AES) + CBC mode with PKCS#7 padding.
 * 256-bit key (32 bytes), 128-bit block (16 bytes), 128-bit IV.
 *
 * Usage:
 *   uint8_t key[32], iv[16];
 *   random_bytes(key, 32);
 *   random_bytes(iv, 16);
 *
 *   // Encrypt (output is padded to 16-byte boundary)
 *   uint8_t ciphertext[MAX_SIZE];
 *   int ct_len = aes256_cbc_encrypt(key, iv, plaintext, pt_len, ciphertext, sizeof(ciphertext));
 *
 *   // Decrypt (removes PKCS#7 padding)
 *   uint8_t recovered[MAX_SIZE];
 *   int pt_len2 = aes256_cbc_decrypt(key, iv, ciphertext, ct_len, recovered, sizeof(recovered));
 */

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE   32  /* AES-256 */

/*
 * Encrypt plaintext using AES-256-CBC with PKCS#7 padding.
 * Returns ciphertext length (always multiple of 16), or -1 on error.
 * out_buf must be at least ((pt_len / 16) + 1) * 16 bytes.
 */
int aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const void* plaintext, size_t pt_len,
                       void* out_buf, size_t out_max);

/*
 * Decrypt ciphertext using AES-256-CBC, removing PKCS#7 padding.
 * Returns plaintext length, or -1 on error (bad padding, etc).
 * ct_len must be a multiple of 16.
 */
int aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const void* ciphertext, size_t ct_len,
                       void* out_buf, size_t out_max);

/*
 * Raw AES-256 single-block encrypt (ECB mode, no padding).
 * Used internally and for building other modes.
 */
void aes256_encrypt_block(const uint8_t key[32],
                          const uint8_t in[16], uint8_t out[16]);

/*
 * Raw AES-256 single-block decrypt (ECB mode, no padding).
 */
void aes256_decrypt_block(const uint8_t key[32],
                          const uint8_t in[16], uint8_t out[16]);

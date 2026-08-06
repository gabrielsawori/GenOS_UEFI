#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * ChaCha20 — RFC 8439 Stream Cipher.
 *
 * Stream cipher yang sangat cepat pada CPU tanpa AES-NI.
 * Digunakan sebagai core generator untuk CSPRNG dan untuk
 * enkripsi bulk data yang membutuhkan kecepatan tinggi.
 *
 * Key: 256-bit (32 byte)
 * Nonce: 96-bit (12 byte)
 * Counter: 32-bit (mulai dari 0 atau 1)
 *
 * Karena stream cipher, encrypt = decrypt (XOR operation).
 */

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

typedef struct {
    uint32_t state[16];     /* 4x4 matrix: constant + key + counter + nonce */
    uint8_t  keystream[CHACHA20_BLOCK_SIZE]; /* Buffer output */
    uint32_t ks_pos;        /* Posisi dalam keystream buffer */
} chacha20_ctx_t;

/*
 * Inisialisasi konteks ChaCha20.
 * key: 32 byte, nonce: 12 byte, counter: awal (biasanya 0 atau 1).
 */
void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32],
                   const uint8_t nonce[12], uint32_t counter);

/*
 * Encrypt/decrypt data secara in-place (XOR dengan keystream).
 * Karena ChaCha20 adalah stream cipher, operasi encrypt dan decrypt
 * identik: ciphertext = plaintext ⊕ keystream.
 */
void chacha20_crypt(chacha20_ctx_t *ctx, uint8_t *data, size_t len);

/*
 * Generate raw keystream bytes (untuk CSPRNG).
 * Output disimpan ke buf tanpa XOR.
 */
void chacha20_keystream(chacha20_ctx_t *ctx, uint8_t *buf, size_t len);

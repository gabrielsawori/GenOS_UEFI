#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * AES-256-CBC — Advanced Encryption Standard (256-bit key, CBC mode).
 *
 * Enkripsi simetris standar industri. Digunakan untuk mengenkripsi
 * data sensitif seperti kredensial, file terenkripsi, dan komunikasi
 * antar proses yang memerlukan kerahasiaan.
 *
 * Mode: CBC (Cipher Block Chaining) — setiap blok ciphertext bergantung
 *       pada blok sebelumnya, sehingga pattern dalam plaintext tersembunyi.
 *
 * Key:  256-bit (32 byte)
 * IV:   128-bit (16 byte) — harus unik dan random untuk setiap operasi
 * Block: 128-bit (16 byte)
 */

#define AES_BLOCK_SIZE    16    /* 128 bits */
#define AES256_KEY_SIZE   32    /* 256 bits */
#define AES256_ROUNDS     14    /* AES-256 uses 14 rounds */
#define AES256_EXPANDED   240   /* 4 * (14+1) * 4 = 240 bytes */

typedef struct {
    uint8_t round_keys[AES256_EXPANDED]; /* Expanded key schedule */
} aes256_ctx_t;

/*
 * Inisialisasi konteks AES-256 dengan key.
 * Melakukan key expansion (key schedule) sekali.
 */
void aes256_init(aes256_ctx_t *ctx, const uint8_t key[32]);

/*
 * Encrypt data secara in-place menggunakan AES-256-CBC.
 *
 * @param ctx:  Konteks yang sudah diinisialisasi
 * @param iv:   Initialization Vector (16 byte) — HARUS random dan unik
 * @param data: Buffer data (harus kelipatan 16 byte / AES_BLOCK_SIZE)
 * @param len:  Panjang data (kelipatan 16)
 *
 * CATATAN: Caller bertanggung jawab atas padding (PKCS7).
 * Gunakan aes256_pad_size() untuk menghitung ukuran setelah padding.
 */
void aes256_cbc_encrypt(aes256_ctx_t *ctx, const uint8_t iv[16],
                        uint8_t *data, size_t len);

/*
 * Decrypt data secara in-place menggunakan AES-256-CBC.
 */
void aes256_cbc_decrypt(aes256_ctx_t *ctx, const uint8_t iv[16],
                        uint8_t *data, size_t len);

/*
 * Hitung ukuran data setelah PKCS7 padding.
 * Selalu menambah minimal 1 byte padding.
 */
static inline size_t aes256_pad_size(size_t data_len) {
    return ((data_len / AES_BLOCK_SIZE) + 1) * AES_BLOCK_SIZE;
}

/*
 * Aplikasikan PKCS7 padding ke buffer.
 * @param data:     Buffer (harus cukup besar untuk padded size)
 * @param data_len: Panjang data asli
 * @return: Panjang setelah padding
 */
size_t aes256_pkcs7_pad(uint8_t *data, size_t data_len);

/*
 * Hapus PKCS7 padding dan kembalikan panjang data asli.
 * @param data: Buffer yang sudah di-decrypt
 * @param len:  Panjang padded data
 * @return: Panjang data asli (atau 0 jika padding invalid)
 */
size_t aes256_pkcs7_unpad(const uint8_t *data, size_t len);

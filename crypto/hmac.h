#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * HMAC-SHA256 — Keyed-Hash Message Authentication Code.
 *
 * Menghasilkan Message Authentication Code (MAC) 256-bit menggunakan
 * SHA-256 sebagai hash function internal. Memastikan integritas dan
 * autentisitas data — penerima dapat memverifikasi bahwa data belum
 * diubah dan pengirim memiliki shared secret key.
 *
 * Standar: RFC 2104 + FIPS 198-1
 */

#define HMAC_SHA256_SIZE 32   /* Output: 256-bit MAC */

/*
 * hmac_sha256 — Hitung HMAC-SHA256 dari data dengan key tertentu.
 *
 * @param key:     Shared secret key
 * @param key_len: Panjang key dalam byte
 * @param data:    Data yang akan di-authenticate
 * @param data_len: Panjang data dalam byte
 * @param mac:     Output buffer (minimal 32 byte)
 */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t *mac);

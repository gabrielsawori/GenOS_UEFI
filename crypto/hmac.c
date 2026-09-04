/*
 * HMAC-SHA256 — RFC 2104 Keyed-Hash Message Authentication Code
 * PBKDF2-HMAC-SHA256 — RFC 8018 Password-Based Key Derivation
 *
 * Pure C, no dependencies beyond sha256.h.
 */
#include "hmac.h"
#include "sha256.h"

void hmac_sha256(const void* key, size_t key_len,
                 const void* data, size_t data_len,
                 uint8_t mac[32])
{
    uint8_t k_pad[SHA256_BLOCK_SIZE];
    uint8_t tk[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;

    /* If key is longer than block size, hash it first */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256_hash(key, key_len, tk);
        key = tk;
        key_len = SHA256_DIGEST_SIZE;
    }

    /* Prepare inner padded key: K XOR 0x36 */
    for (size_t i = 0; i < key_len; i++)
        k_pad[i] = ((const uint8_t*)key)[i] ^ 0x36;
    for (size_t i = key_len; i < SHA256_BLOCK_SIZE; i++)
        k_pad[i] = 0x36;

    /* Inner hash: H(K^ipad || data) */
    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, mac);

    /* Prepare outer padded key: K XOR 0x5C */
    for (size_t i = 0; i < key_len; i++)
        k_pad[i] = ((const uint8_t*)key)[i] ^ 0x5C;
    for (size_t i = key_len; i < SHA256_BLOCK_SIZE; i++)
        k_pad[i] = 0x5C;

    /* Outer hash: H(K^opad || inner_hash) */
    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, mac, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, mac);
}

void pbkdf2_hmac_sha256(const void* password, size_t pass_len,
                        const void* salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t* dk, size_t dk_len)
{
    /*
     * PBKDF2 with single block output (dk_len <= 32).
     * For GenOS password hashing, 32 bytes is always sufficient.
     *
     * U_1 = HMAC(password, salt || INT_32_BE(1))
     * U_i = HMAC(password, U_{i-1})
     * DK  = U_1 XOR U_2 XOR ... XOR U_c
     */
    if (dk_len > 32) dk_len = 32;

    uint8_t U[32], T[32];

    /* U_1 = HMAC(password, salt || 0x00000001) */
    /* Build (salt || block_index) in a temp buffer */
    uint8_t salt_block[128 + 4]; /* salt up to 128 bytes + 4-byte index */
    size_t sb_len = salt_len;
    if (sb_len > 128) sb_len = 128;
    for (size_t i = 0; i < sb_len; i++)
        salt_block[i] = ((const uint8_t*)salt)[i];
    /* Append block index 1 as big-endian uint32 */
    salt_block[sb_len]   = 0;
    salt_block[sb_len+1] = 0;
    salt_block[sb_len+2] = 0;
    salt_block[sb_len+3] = 1;

    hmac_sha256(password, pass_len, salt_block, sb_len + 4, U);
    for (size_t i = 0; i < 32; i++) T[i] = U[i];

    /* U_2 .. U_c */
    for (uint32_t iter = 1; iter < iterations; iter++) {
        hmac_sha256(password, pass_len, U, 32, U);
        for (size_t i = 0; i < 32; i++) T[i] ^= U[i];
    }

    /* Copy derived key */
    for (size_t i = 0; i < dk_len; i++)
        dk[i] = T[i];
}

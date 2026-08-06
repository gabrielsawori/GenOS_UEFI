#include "aes.h"
#include "../kernel/utils.h"

/*
 * AES-256 Implementation — FIPS 197
 *
 * Implementasi murni software dengan lookup table (T-tables approach
 * TIDAK digunakan untuk menghindari cache-timing side channels).
 * Menggunakan S-box + ShiftRows + MixColumns secara eksplisit.
 */

/* === AES S-Box (SubBytes) === */
static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/* === AES Inverse S-Box (InvSubBytes) for decryption === */
static const uint8_t inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38,
    0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d,
    0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2,
    0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
    0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a,
    0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea,
    0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20,
    0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31,
    0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0,
    0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26,
    0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

/* AES Round Constants (Rcon) for key expansion */
static const uint8_t rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* GF(2^8) multiplication helpers for MixColumns */
static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* === Key Expansion (Key Schedule) for AES-256 === */
static void aes256_key_expansion(const uint8_t key[32], uint8_t *rk) {
    int i;
    /* Copy original key as first 8 words */
    memcpy(rk, key, 32);

    /* AES-256: Nk=8, Nr=14, total 60 words = 240 bytes */
    for (i = 8; i < 60; i++) {
        uint8_t temp[4];
        temp[0] = rk[(i-1)*4 + 0];
        temp[1] = rk[(i-1)*4 + 1];
        temp[2] = rk[(i-1)*4 + 2];
        temp[3] = rk[(i-1)*4 + 3];

        if (i % 8 == 0) {
            /* RotWord + SubWord + Rcon */
            uint8_t t = temp[0];
            temp[0] = sbox[temp[1]] ^ rcon[i/8];
            temp[1] = sbox[temp[2]];
            temp[2] = sbox[temp[3]];
            temp[3] = sbox[t];
        } else if (i % 8 == 4) {
            /* Extra SubWord for AES-256 */
            temp[0] = sbox[temp[0]];
            temp[1] = sbox[temp[1]];
            temp[2] = sbox[temp[2]];
            temp[3] = sbox[temp[3]];
        }

        rk[i*4 + 0] = rk[(i-8)*4 + 0] ^ temp[0];
        rk[i*4 + 1] = rk[(i-8)*4 + 1] ^ temp[1];
        rk[i*4 + 2] = rk[(i-8)*4 + 2] ^ temp[2];
        rk[i*4 + 3] = rk[(i-8)*4 + 3] ^ temp[3];
    }
}

/* === AES Block Operations (single 128-bit block) === */

static void add_round_key(uint8_t state[16], const uint8_t *rk, int round) {
    for (int i = 0; i < 16; i++) {
        state[i] ^= rk[round * 16 + i];
    }
}

static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = sbox[state[i]];
    }
}

static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = inv_sbox[state[i]];
    }
}

/*
 * ShiftRows — Geser baris ke kiri:
 *   Row 0: no shift
 *   Row 1: shift left 1
 *   Row 2: shift left 2
 *   Row 3: shift left 3
 *
 * State disimpan column-major: state[col*4 + row]
 */
static void shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* Row 1 */
    t = state[1]; state[1] = state[5]; state[5] = state[9];
    state[9] = state[13]; state[13] = t;
    /* Row 2 */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* Row 3 */
    t = state[15]; state[15] = state[11]; state[11] = state[7];
    state[7] = state[3]; state[3] = t;
}

static void inv_shift_rows(uint8_t state[16]) {
    uint8_t t;
    /* Row 1 */
    t = state[13]; state[13] = state[9]; state[9] = state[5];
    state[5] = state[1]; state[1] = t;
    /* Row 2 */
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    /* Row 3 */
    t = state[3]; state[3] = state[7]; state[7] = state[11];
    state[11] = state[15]; state[15] = t;
}

static void mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = state[i], a1 = state[i+1];
        uint8_t a2 = state[i+2], a3 = state[i+3];
        state[i]   = gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3;
        state[i+1] = a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3;
        state[i+2] = a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3);
        state[i+3] = gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2);
    }
}

static void inv_mix_columns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = state[i], a1 = state[i+1];
        uint8_t a2 = state[i+2], a3 = state[i+3];
        state[i]   = gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9);
        state[i+1] = gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13);
        state[i+2] = gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11);
        state[i+3] = gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14);
    }
}

/* Encrypt single block */
static void aes256_encrypt_block(const aes256_ctx_t *ctx, uint8_t block[16]) {
    add_round_key(block, ctx->round_keys, 0);
    for (int r = 1; r < AES256_ROUNDS; r++) {
        sub_bytes(block);
        shift_rows(block);
        mix_columns(block);
        add_round_key(block, ctx->round_keys, r);
    }
    /* Last round (no MixColumns) */
    sub_bytes(block);
    shift_rows(block);
    add_round_key(block, ctx->round_keys, AES256_ROUNDS);
}

/* Decrypt single block */
static void aes256_decrypt_block(const aes256_ctx_t *ctx, uint8_t block[16]) {
    add_round_key(block, ctx->round_keys, AES256_ROUNDS);
    for (int r = AES256_ROUNDS - 1; r >= 1; r--) {
        inv_shift_rows(block);
        inv_sub_bytes(block);
        add_round_key(block, ctx->round_keys, r);
        inv_mix_columns(block);
    }
    /* Last round (no InvMixColumns) */
    inv_shift_rows(block);
    inv_sub_bytes(block);
    add_round_key(block, ctx->round_keys, 0);
}

/* === Public API === */

void aes256_init(aes256_ctx_t *ctx, const uint8_t key[32]) {
    aes256_key_expansion(key, ctx->round_keys);
}

void aes256_cbc_encrypt(aes256_ctx_t *ctx, const uint8_t iv[16],
                        uint8_t *data, size_t len) {
    uint8_t prev[AES_BLOCK_SIZE];
    memcpy(prev, iv, AES_BLOCK_SIZE);

    for (size_t offset = 0; offset + AES_BLOCK_SIZE <= len; offset += AES_BLOCK_SIZE) {
        /* XOR plaintext block with previous ciphertext (or IV for first) */
        for (int i = 0; i < AES_BLOCK_SIZE; i++) {
            data[offset + i] ^= prev[i];
        }
        /* Encrypt block */
        aes256_encrypt_block(ctx, data + offset);
        /* Save ciphertext for chaining */
        memcpy(prev, data + offset, AES_BLOCK_SIZE);
    }
}

void aes256_cbc_decrypt(aes256_ctx_t *ctx, const uint8_t iv[16],
                        uint8_t *data, size_t len) {
    uint8_t prev[AES_BLOCK_SIZE];
    uint8_t saved[AES_BLOCK_SIZE];
    memcpy(prev, iv, AES_BLOCK_SIZE);

    for (size_t offset = 0; offset + AES_BLOCK_SIZE <= len; offset += AES_BLOCK_SIZE) {
        /* Save current ciphertext (needed for next XOR) */
        memcpy(saved, data + offset, AES_BLOCK_SIZE);
        /* Decrypt block */
        aes256_decrypt_block(ctx, data + offset);
        /* XOR with previous ciphertext (or IV) */
        for (int i = 0; i < AES_BLOCK_SIZE; i++) {
            data[offset + i] ^= prev[i];
        }
        /* Previous = saved ciphertext */
        memcpy(prev, saved, AES_BLOCK_SIZE);
    }
}

size_t aes256_pkcs7_pad(uint8_t *data, size_t data_len) {
    uint8_t pad_val = AES_BLOCK_SIZE - (data_len % AES_BLOCK_SIZE);
    for (uint8_t i = 0; i < pad_val; i++) {
        data[data_len + i] = pad_val;
    }
    return data_len + pad_val;
}

size_t aes256_pkcs7_unpad(const uint8_t *data, size_t len) {
    if (len == 0 || len % AES_BLOCK_SIZE != 0) return 0;
    uint8_t pad_val = data[len - 1];
    if (pad_val == 0 || pad_val > AES_BLOCK_SIZE) return 0;
    /* Verify all padding bytes */
    for (uint8_t i = 0; i < pad_val; i++) {
        if (data[len - 1 - i] != pad_val) return 0;
    }
    return len - pad_val;
}

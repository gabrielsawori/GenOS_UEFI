#include "chacha20.h"
#include "../kernel/utils.h"

/*
 * ChaCha20 Implementation — RFC 8439
 *
 * Implementasi murni software. ChaCha20 sangat cocok untuk kernel OS
 * karena performa tinggi di CPU tanpa instruksi AES-NI, dan implementasi
 * yang sederhana serta aman dari timing side-channels.
 */

/* ChaCha20 constants: "expand 32-byte k" in little-endian */
#define CHACHA20_CONST0 0x61707865
#define CHACHA20_CONST1 0x3320646e
#define CHACHA20_CONST2 0x79622d32
#define CHACHA20_CONST3 0x6b206574

/* Little-endian pack/unpack */
static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * Quarter Round — Inti dari ChaCha20.
 * Mengoperasikan 4 elemen state matrix dalam satu iterasi.
 */
#define QR(a, b, c, d) do {          \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d <<  8) | (d >> 24); \
    c += d; b ^= c; b = (b <<  7) | (b >> 25); \
} while(0)

/*
 * chacha20_block — Generate 64 byte keystream dari state.
 * 20 rounds (10 double-rounds), lalu add original state.
 */
static void chacha20_block(const uint32_t input[16], uint8_t output[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = input[i];

    /* 20 rounds = 10 double-rounds (column + diagonal) */
    for (int i = 0; i < 10; i++) {
        /* Column rounds */
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        /* Diagonal rounds */
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }

    /* Add original state */
    for (int i = 0; i < 16; i++) {
        put_le32(output + i * 4, x[i] + input[i]);
    }
}

/* === Public API === */

void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32],
                   const uint8_t nonce[12], uint32_t counter) {
    /* Row 0: Constants */
    ctx->state[0] = CHACHA20_CONST0;
    ctx->state[1] = CHACHA20_CONST1;
    ctx->state[2] = CHACHA20_CONST2;
    ctx->state[3] = CHACHA20_CONST3;

    /* Row 1-2: Key (8 x 32-bit words) */
    for (int i = 0; i < 8; i++) {
        ctx->state[4 + i] = le32(key + i * 4);
    }

    /* Row 3: Counter + Nonce */
    ctx->state[12] = counter;
    ctx->state[13] = le32(nonce);
    ctx->state[14] = le32(nonce + 4);
    ctx->state[15] = le32(nonce + 8);

    /* Invalidate keystream buffer — force generation on first use */
    ctx->ks_pos = CHACHA20_BLOCK_SIZE;
}

void chacha20_crypt(chacha20_ctx_t *ctx, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        /* Generate new keystream block if needed */
        if (ctx->ks_pos >= CHACHA20_BLOCK_SIZE) {
            chacha20_block(ctx->state, ctx->keystream);
            ctx->state[12]++;  /* Increment counter */
            ctx->ks_pos = 0;
        }
        data[i] ^= ctx->keystream[ctx->ks_pos++];
    }
}

void chacha20_keystream(chacha20_ctx_t *ctx, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (ctx->ks_pos >= CHACHA20_BLOCK_SIZE) {
            chacha20_block(ctx->state, ctx->keystream);
            ctx->state[12]++;
            ctx->ks_pos = 0;
        }
        buf[i] = ctx->keystream[ctx->ks_pos++];
    }
}

/*
 * SHA-256 — FIPS 180-4 Cryptographic Hash Function
 *
 * Pure C implementation for bare-metal (no libc dependency).
 * Processes messages in 512-bit (64-byte) blocks.
 *
 * References:
 *   NIST FIPS 180-4: Secure Hash Standard (SHS)
 *   RFC 6234: US Secure Hash Algorithms
 */
#include "sha256.h"

/* SHA-256 round constants (first 32 bits of fractional parts of cube roots of first 64 primes) */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* Bitwise operations */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/*
 * sha256_transform() — Process one 64-byte block.
 * This is the compression function at the heart of SHA-256.
 */
static void sha256_transform(sha256_ctx_t* ctx, const uint8_t block[64]) {
    uint32_t W[64], a, b, c, d, e, f, g, h, t1, t2;

    /* Prepare message schedule W[0..63] */
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];
    }

    /* Initialize working variables */
    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    /* 64 rounds of compression */
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    /* Add compressed chunk to hash state */
    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t* ctx) {
    /* Initial hash values (first 32 bits of fractional parts of square roots of first 8 primes) */
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;

    ctx->bitcount += (uint64_t)len * 8;

    /* If we have buffered data, try to fill the block */
    if (ctx->buflen > 0) {
        uint32_t need = 64 - ctx->buflen;
        if (len < need) {
            for (size_t i = 0; i < len; i++)
                ctx->buffer[ctx->buflen + i] = p[i];
            ctx->buflen += (uint32_t)len;
            return;
        }
        for (uint32_t i = 0; i < need; i++)
            ctx->buffer[ctx->buflen + i] = p[i];
        sha256_transform(ctx, ctx->buffer);
        p += need;
        len -= need;
        ctx->buflen = 0;
    }

    /* Process full blocks directly */
    while (len >= 64) {
        sha256_transform(ctx, p);
        p += 64;
        len -= 64;
    }

    /* Buffer remaining bytes */
    for (size_t i = 0; i < len; i++)
        ctx->buffer[i] = p[i];
    ctx->buflen = (uint32_t)len;
}

void sha256_final(sha256_ctx_t* ctx, uint8_t digest[32]) {
    /* Pad message: append 0x80, then zeros, then 64-bit big-endian bit count */
    uint32_t pad_start = ctx->buflen;
    ctx->buffer[pad_start++] = 0x80;

    /* If not enough room for 8-byte length, fill and transform */
    if (pad_start > 56) {
        while (pad_start < 64)
            ctx->buffer[pad_start++] = 0;
        sha256_transform(ctx, ctx->buffer);
        pad_start = 0;
    }

    /* Zero-fill up to byte 56 */
    while (pad_start < 56)
        ctx->buffer[pad_start++] = 0;

    /* Append bit count as 64-bit big-endian */
    uint64_t bits = ctx->bitcount;
    ctx->buffer[56] = (uint8_t)(bits >> 56);
    ctx->buffer[57] = (uint8_t)(bits >> 48);
    ctx->buffer[58] = (uint8_t)(bits >> 40);
    ctx->buffer[59] = (uint8_t)(bits >> 32);
    ctx->buffer[60] = (uint8_t)(bits >> 24);
    ctx->buffer[61] = (uint8_t)(bits >> 16);
    ctx->buffer[62] = (uint8_t)(bits >> 8);
    ctx->buffer[63] = (uint8_t)(bits);
    sha256_transform(ctx, ctx->buffer);

    /* Output digest in big-endian */
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hash(const void* data, size_t len, uint8_t digest[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

void sha256_to_hex(const uint8_t digest[32], char hex[65]) {
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = hexchars[(digest[i] >> 4) & 0xF];
        hex[i*2+1] = hexchars[digest[i] & 0xF];
    }
    hex[64] = '\0';
}

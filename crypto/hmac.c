#include "hmac.h"
#include "sha256.h"
#include "../kernel/utils.h"

/*
 * HMAC-SHA256 Implementation — RFC 2104
 *
 * HMAC(K, m) = H((K' ⊕ opad) || H((K' ⊕ ipad) || m))
 *
 * Di mana:
 *   K' = key (di-pad ke block_size jika pendek, atau di-hash jika panjang)
 *   ipad = 0x36 diulang block_size kali
 *   opad = 0x5c diulang block_size kali
 */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t *mac) {
    uint8_t k_prime[SHA256_BLOCK_SIZE];
    uint8_t i_key_pad[SHA256_BLOCK_SIZE];
    uint8_t o_key_pad[SHA256_BLOCK_SIZE];
    sha256_ctx_t ctx;
    uint8_t inner_hash[SHA256_DIGEST_SIZE];

    /* Step 1: Derive K' dari key asli */
    if (key_len > SHA256_BLOCK_SIZE) {
        /* Key terlalu panjang → hash dulu */
        sha256_hash(key, key_len, k_prime);
        memset(k_prime + SHA256_DIGEST_SIZE, 0,
               SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        /* Key pendek → pad dengan zeros */
        memcpy(k_prime, key, key_len);
        memset(k_prime + key_len, 0, SHA256_BLOCK_SIZE - key_len);
    }

    /* Step 2: Hitung inner pad dan outer pad */
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        i_key_pad[i] = k_prime[i] ^ 0x36;
        o_key_pad[i] = k_prime[i] ^ 0x5c;
    }

    /* Step 3: Inner hash = H(ipad || message) */
    sha256_init(&ctx);
    sha256_update(&ctx, i_key_pad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner_hash);

    /* Step 4: Outer hash = H(opad || inner_hash) */
    sha256_init(&ctx);
    sha256_update(&ctx, o_key_pad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner_hash, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, mac);

    /* Bersihkan data sensitif dari stack */
    memset(k_prime, 0, sizeof(k_prime));
    memset(i_key_pad, 0, sizeof(i_key_pad));
    memset(o_key_pad, 0, sizeof(o_key_pad));
    memset(inner_hash, 0, sizeof(inner_hash));
}

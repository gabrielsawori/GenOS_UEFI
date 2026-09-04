#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * GenOS User-Space Cryptography & Security API
 *
 * All functions use syscalls to access kernel crypto subsystem.
 * These are safe to call from Ring 3 applications.
 */

/* === Random Number Generation (syscall 37) === */

/* Fill buffer with cryptographically random bytes */
int crypto_random(void* buf, size_t len);

/* === SHA-256 Hashing (syscall 38) === */

/* Hash data and output 32-byte digest; returns 0=OK */
int crypto_sha256(const void* data, size_t len, uint8_t digest[32]);

/* === AES-256-CBC Encryption (syscall 39) === */

/*
 * Encrypt data using AES-256-CBC.
 * key must be 32 bytes, iv must be 16 bytes.
 * Output is written to out_buf (must be large enough for padded data).
 * Args packed: arg1 = key_ptr, arg2 = (iv_ptr << 32 | data_ptr), arg3 = len
 * Returns: ciphertext length, or -1 on error.
 *
 * Note: Due to 3-arg syscall limit, this uses a parameter struct.
 */
typedef struct {
    const uint8_t* key;      /* 32-byte AES key */
    const uint8_t* iv;       /* 16-byte IV */
    const void*    input;    /* plaintext data */
    size_t         in_len;   /* input length */
    void*          output;   /* output buffer */
    size_t         out_max;  /* output buffer size */
} crypto_aes_params_t;

int crypto_aes_encrypt(crypto_aes_params_t* params);

/* === AES-256-CBC Decryption (syscall 40) === */

int crypto_aes_decrypt(crypto_aes_params_t* params);

/* === Authentication (syscall 41) === */

/* Login with username and password; returns UID >= 0, or -1 on failure */
int crypto_login(const char* username, const char* password);

/* === User Info (syscall 42) === */

/* Get current task's UID and write username to buf; returns UID */
int crypto_whoami(char* username_buf, size_t buf_len);

/* === Keystore (syscall 44) === */

#define KEYSTORE_OP_SET    0
#define KEYSTORE_OP_GET    1
#define KEYSTORE_OP_DELETE 2
#define KEYSTORE_OP_COUNT  3
#define KEYSTORE_OP_INIT   4

typedef struct {
    int         op;         /* KEYSTORE_OP_* */
    const char* name;       /* Key name */
    void*       value;      /* Value buffer (in or out) */
    size_t      val_len;    /* Value length (in) or buffer size (out) */
    int*        out_len;    /* Actual length written (for GET) */
} crypto_keystore_params_t;

int crypto_keystore(crypto_keystore_params_t* params);

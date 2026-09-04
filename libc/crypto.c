/*
 * GenOS User-Space Cryptography Syscall Wrappers
 */
#include "crypto.h"
#include "syscall.h"

int crypto_random(void* buf, size_t len) {
    return (int)syscall(37, (uint64_t)buf, (uint64_t)len, 0);
}

int crypto_sha256(const void* data, size_t len, uint8_t digest[32]) {
    return (int)syscall(38, (uint64_t)data, (uint64_t)len, (uint64_t)digest);
}

int crypto_aes_encrypt(crypto_aes_params_t* params) {
    return (int)syscall(39, (uint64_t)params, 0, 0);
}

int crypto_aes_decrypt(crypto_aes_params_t* params) {
    return (int)syscall(40, (uint64_t)params, 0, 0);
}

int crypto_login(const char* username, const char* password) {
    return (int)syscall(41, (uint64_t)username, (uint64_t)password, 0);
}

int crypto_whoami(char* username_buf, size_t buf_len) {
    return (int)syscall(42, (uint64_t)username_buf, (uint64_t)buf_len, 0);
}

int crypto_keystore(crypto_keystore_params_t* params) {
    return (int)syscall(44, (uint64_t)params, 0, 0);
}

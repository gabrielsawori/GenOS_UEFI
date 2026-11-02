#include "syscall_user.h"
#include "uaccess.h"
#include "preempt.h"
#include "../mm/heap.h"
#include "../crypto/random.h"
#include "../crypto/sha256.h"
#include "../crypto/aes.h"
#include "../security/keystore.h"

/* Bound plaintext allocations; ciphertext has one extra padding block. */
#define USER_AES_MAX 65536

static void wipe(void* ptr, size_t len) {
    volatile uint8_t* bytes = ptr;
    while (len--) *bytes++ = 0;
}

int sys_user_random(void* user_buf, size_t len) {
    uint8_t buffer[4096];
    if (!len || len > sizeof(buffer) || !user_access_ok(user_buf, len, 1)) return -1;
    {
        PREEMPT_GUARD;
        random_bytes(buffer, len);
    }
    int result = copy_to_user(user_buf, buffer, len);
    wipe(buffer, len);
    return result;
}

int sys_user_sha256(const void* data, size_t len, void* user_digest) {
    uint8_t buffer[1024], digest[SHA256_DIGEST_SIZE];
    sha256_ctx_t context;
    if (!user_access_ok(data, len, 0) ||
        !user_access_ok(user_digest, sizeof(digest), 1)) return -1;
    sha256_init(&context);
    int result = -1;
    for (size_t offset = 0; offset < len;) {
        size_t chunk = len - offset;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (copy_from_user(buffer, (const void*)((uint64_t)data + offset), chunk)) goto out;
        sha256_update(&context, buffer, chunk);
        offset += chunk;
    }
    sha256_final(&context, digest);
    result = copy_to_user(user_digest, digest, sizeof(digest));
out:
    wipe(buffer, sizeof(buffer));
    wipe(digest, sizeof(digest));
    wipe(&context, sizeof(context));
    return result;
}

int sys_user_aes(const void* user_params, int decrypt) {
    /* Do not let group exit reap this task while it owns heap temporaries. */
    PREEMPT_GUARD;
    struct {
        const uint8_t* key;
        const uint8_t* iv;
        const void* input;
        size_t in_len;
        void* output;
        size_t out_max;
    } p;
    uint8_t key[AES_KEY_SIZE] = {0}, iv[AES_BLOCK_SIZE] = {0};
    uint8_t *input = NULL, *output = NULL;
    int result = -1;
    if (copy_from_user(&p, user_params, sizeof(p)) ||
        p.in_len > USER_AES_MAX + (decrypt ? AES_BLOCK_SIZE : 0) ||
        (decrypt && (!p.in_len || p.in_len % AES_BLOCK_SIZE))) return -1;
    size_t output_len = decrypt ? p.in_len : (p.in_len / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;
    if (p.out_max < output_len || !user_access_ok(p.input, p.in_len, 0) ||
        !user_access_ok(p.output, output_len, 1)) return -1;
    if (copy_from_user(key, p.key, sizeof(key)) || copy_from_user(iv, p.iv, sizeof(iv))) goto out;
    input = kmalloc(p.in_len ? p.in_len : 1);
    output = kmalloc(output_len);
    if (!input || !output || copy_from_user(input, p.input, p.in_len)) goto out;
    result = decrypt ? aes256_cbc_decrypt(key, iv, input, p.in_len, output, output_len)
                     : aes256_cbc_encrypt(key, iv, input, p.in_len, output, output_len);
    if (result >= 0 && copy_to_user(p.output, output, (size_t)result)) result = -1;
out:
    if (input) { wipe(input, p.in_len); kfree(input); }
    if (output) { wipe(output, output_len); kfree(output); }
    wipe(key, sizeof(key));
    wipe(iv, sizeof(iv));
    return result;
}

int sys_user_keystore(const void* user_params) {
    struct {
        int op;
        const char* name;
        void* value;
        size_t val_len;
        int* out_len;
    } p;
    char name[USER_STRING_MAX];
    uint8_t value[KEYSTORE_VAL_MAX] = {0};
    int length = 0, result = -1;
    if (copy_from_user(&p, user_params, sizeof(p))) return -1;
    if (p.op == 3) {
        PREEMPT_GUARD;
        return keystore_count();
    }
    if (p.op < 0 || p.op > 4 ||
        copy_user_string(name, p.name, p.op == 4 ? sizeof(name) : KEYSTORE_KEY_MAX) < 0)
        goto out;
    /* The keystore is shared. User inputs are snapshots before mutation;
     * output CoW is prepared before the read under the same BSP guard. */
    {
        PREEMPT_GUARD;
        switch (p.op) {
        case 0:
            if (p.val_len > KEYSTORE_VAL_MAX - AES_BLOCK_SIZE ||
                copy_from_user(value, p.value, p.val_len)) break;
            result = keystore_set(name, value, p.val_len);
            break;
        case 1: {
            size_t capacity = p.val_len < sizeof(value) ? p.val_len : sizeof(value);
            if (user_write_prepare(p.value, capacity) ||
                (p.out_len && user_write_prepare(p.out_len, sizeof(length)))) break;
            result = keystore_get(name, value, capacity, &length);
            if (!result && (copy_to_user(p.value, value, (size_t)length) ||
                            (p.out_len && copy_to_user(p.out_len, &length, sizeof(length)))))
                result = -1;
            break;
        }
        case 2: result = keystore_delete(name); break;
        case 4: keystore_init(name); result = 0; break;
        }
    }
out:
    wipe(name, sizeof(name));
    wipe(value, sizeof(value));
    return result;
}

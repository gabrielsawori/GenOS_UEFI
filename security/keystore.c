/*
 * GenOS Encrypted Keystore
 *
 * Stores key-value pairs with AES-256-CBC encryption.
 * The master encryption key is derived from a passphrase
 * using PBKDF2-HMAC-SHA256.
 *
 * Each entry stores: name (plaintext), encrypted value, IV.
 * Values are decrypted on-demand during retrieval.
 */
#include "keystore.h"
#include "../crypto/aes.h"
#include "../crypto/hmac.h"
#include "../crypto/random.h"
#include "../drivers/serial.h"

typedef struct {
    char    name[KEYSTORE_KEY_MAX];
    uint8_t encrypted[KEYSTORE_VAL_MAX]; /* AES-256-CBC ciphertext */
    int     enc_len;                     /* Length of ciphertext */
    uint8_t iv[16];                      /* Per-entry IV */
    uint8_t active;
} keystore_entry_t;

static keystore_entry_t entries[KEYSTORE_MAX_ENTRIES];
static uint8_t master_key[32];  /* Derived AES-256 key */
static int initialized = 0;

static int str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

void keystore_init(const char* master_passphrase) {
    /* Clear all entries */
    for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++)
        entries[i].active = 0;

    /* Derive master key from passphrase using PBKDF2 */
    uint8_t salt[] = "GenOS_Keystore_Salt_v1"; /* Fixed salt for keystore */
    pbkdf2_hmac_sha256(master_passphrase, (size_t)str_len(master_passphrase),
                       salt, sizeof(salt) - 1,
                       10, /* Reduced from 2000: prevents boot-time hang */
                       master_key, 32);

    initialized = 1;
    serial_write_string("[OK] Keystore initialized (AES-256 encrypted)\n");
}

int keystore_set(const char* name, const void* value, size_t val_len) {
    if (!initialized || !name || !value) return -1;
    if (val_len > KEYSTORE_VAL_MAX - 16) return -1; /* Room for padding */

    /* Check if key already exists; update it */
    int idx = -1;
    for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++) {
        if (entries[i].active && str_eq(entries[i].name, name)) {
            idx = i;
            break;
        }
    }

    /* Otherwise find free slot */
    if (idx < 0) {
        for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++) {
            if (!entries[i].active) { idx = i; break; }
        }
    }
    if (idx < 0) return -1; /* Full */

    keystore_entry_t* e = &entries[idx];

    /* Copy name */
    int j;
    for (j = 0; name[j] && j < KEYSTORE_KEY_MAX - 1; j++)
        e->name[j] = name[j];
    e->name[j] = '\0';

    /* Generate random IV for this entry */
    random_bytes(e->iv, 16);

    /* Encrypt value with master key */
    e->enc_len = aes256_cbc_encrypt(master_key, e->iv,
                                    value, val_len,
                                    e->encrypted, KEYSTORE_VAL_MAX);
    if (e->enc_len < 0) return -1;

    e->active = 1;
    return 0;
}

int keystore_get(const char* name, void* out_buf, size_t out_max, int* out_len) {
    if (!initialized || !name) return -1;

    for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++) {
        if (!entries[i].active) continue;
        if (!str_eq(entries[i].name, name)) continue;

        /* Decrypt value */
        int pt_len = aes256_cbc_decrypt(master_key, entries[i].iv,
                                        entries[i].encrypted, (size_t)entries[i].enc_len,
                                        out_buf, out_max);
        if (pt_len < 0) return -1;
        if (out_len) *out_len = pt_len;
        return 0;
    }

    return -1; /* Not found */
}

int keystore_delete(const char* name) {
    if (!initialized) return -1;

    for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++) {
        if (entries[i].active && str_eq(entries[i].name, name)) {
            /* Zero out sensitive data before marking inactive */
            for (int j = 0; j < KEYSTORE_VAL_MAX; j++)
                entries[i].encrypted[j] = 0;
            for (int j = 0; j < 16; j++)
                entries[i].iv[j] = 0;
            entries[i].active = 0;
            return 0;
        }
    }
    return -1;
}

int keystore_count(void) {
    int count = 0;
    for (int i = 0; i < KEYSTORE_MAX_ENTRIES; i++)
        if (entries[i].active) count++;
    return count;
}

int keystore_is_ready(void) {
    return initialized;
}

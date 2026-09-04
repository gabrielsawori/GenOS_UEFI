#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * GenOS Encrypted Keystore
 *
 * In-memory key-value store where values are AES-256-CBC encrypted.
 * Master key is derived from a passphrase at init time.
 *
 * Usage:
 *   keystore_init("my_master_password");
 *   keystore_set("wifi_pass", "secret123", 9);
 *   char buf[64]; int len;
 *   keystore_get("wifi_pass", buf, sizeof(buf), &len);
 */

#define KEYSTORE_MAX_ENTRIES 16
#define KEYSTORE_KEY_MAX     32   /* key name max length */
#define KEYSTORE_VAL_MAX     256  /* encrypted value max length */

/* Initialize keystore with a master passphrase */
void keystore_init(const char* master_passphrase);

/* Store an encrypted key-value pair; returns 0=OK, -1=error */
int keystore_set(const char* name, const void* value, size_t val_len);

/* Retrieve and decrypt a value; returns 0=OK, -1=not found */
int keystore_get(const char* name, void* out_buf, size_t out_max, int* out_len);

/* Delete a key; returns 0=OK, -1=not found */
int keystore_delete(const char* name);

/* Get number of stored entries */
int keystore_count(void);

/* Check if keystore is initialized; returns 1=yes, 0=no */
int keystore_is_ready(void);

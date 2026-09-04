/*
 * GenOS Credential & Authentication System
 *
 * Stores user accounts in kernel memory with PBKDF2-hashed passwords.
 * Passwords are never stored in plaintext — only the derived hash
 * and a random salt are kept.
 *
 * Default accounts created at boot:
 *   root   (UID 0, GID 0)   — password: "root"
 *   mandor (UID 1000, GID 1000) — password: "genos"
 */
#include "cred.h"
#include "../crypto/hmac.h"
#include "../crypto/random.h"
#include "../drivers/serial.h"

static user_account_t users[MAX_USERS];
static uint32_t next_uid = 1001;

/* Simple string comparison */
static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Hash a password with salt using PBKDF2-HMAC-SHA256 */
static void hash_password(const char* password, const uint8_t salt[SALT_SIZE],
                          uint8_t hash_out[HASH_SIZE])
{
    pbkdf2_hmac_sha256(password, (size_t)str_len(password),
                       salt, SALT_SIZE,
                       PBKDF2_ITERATIONS,
                       hash_out, HASH_SIZE);
}

/* Constant-time comparison to prevent timing attacks */
static int secure_compare(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

/* Create a user entry at a specific index */
static void create_user_at(int idx, const char* username, const char* password,
                           uint32_t uid, uint32_t gid)
{
    user_account_t* u = &users[idx];

    /* Copy username */
    int i;
    for (i = 0; username[i] && i < USERNAME_MAX - 1; i++)
        u->username[i] = username[i];
    u->username[i] = '\0';

    u->uid = uid;
    u->gid = gid;
    u->active = 1;

    /* Generate random salt */
    random_bytes(u->salt, SALT_SIZE);

    /* Hash password with salt */
    hash_password(password, u->salt, u->password_hash);
}

void cred_init(void) {
    /* Clear all slots */
    for (int i = 0; i < MAX_USERS; i++)
        users[i].active = 0;

    /* Create default accounts */
    create_user_at(0, "root",   "root",  0,    0);
    create_user_at(1, "mandor", "genos", 1000, 1000);

    serial_write_string("[OK] Credential system initialized (2 users: root, mandor)\n");
}

int cred_authenticate(const char* username, const char* password) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) continue;
        if (!str_eq(users[i].username, username)) continue;

        /* Hash provided password with stored salt */
        uint8_t test_hash[HASH_SIZE];
        hash_password(password, users[i].salt, test_hash);

        /* Constant-time compare */
        if (secure_compare(test_hash, users[i].password_hash, HASH_SIZE)) {
            serial_write_string("[AUTH] Login success: ");
            serial_write_string(username);
            serial_write_string("\n");
            return (int)users[i].uid;
        }

        serial_write_string("[AUTH] Login failed: ");
        serial_write_string(username);
        serial_write_string(" (wrong password)\n");
        return -1;
    }

    serial_write_string("[AUTH] Login failed: user not found: ");
    serial_write_string(username);
    serial_write_string("\n");
    return -1;
}

const char* cred_get_username(uint32_t uid) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && users[i].uid == uid)
            return users[i].username;
    }
    return (void*)0;
}

int cred_get_uid(const char* username) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && str_eq(users[i].username, username))
            return (int)users[i].uid;
    }
    return -1;
}

int cred_change_password(uint32_t uid, const char* new_password) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active || users[i].uid != uid) continue;

        /* Generate new salt */
        random_bytes(users[i].salt, SALT_SIZE);

        /* Hash new password */
        hash_password(new_password, users[i].salt, users[i].password_hash);

        serial_write_string("[AUTH] Password changed for UID ");
        /* Simple UID print */
        char buf[12];
        int n = 0;
        uint32_t v = uid;
        if (v == 0) { buf[n++] = '0'; }
        else {
            char tmp[12]; int t = 0;
            while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            while (t > 0) buf[n++] = tmp[--t];
        }
        buf[n] = '\0';
        serial_write_string(buf);
        serial_write_string("\n");
        return 0;
    }
    return -1;
}

int cred_create_user(const char* username, const char* password, uint32_t gid) {
    /* Check if username already exists */
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && str_eq(users[i].username, username))
            return -1;
    }

    /* Find free slot */
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) {
            create_user_at(i, username, password, next_uid, gid);
            return (int)(next_uid++);
        }
    }

    return -1; /* Table full */
}

int cred_user_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active) count++;
    return count;
}

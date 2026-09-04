#pragma once
#include <stdint.h>

/*
 * GenOS Credential & Authentication System
 *
 * Manages user accounts with PBKDF2-HMAC-SHA256 password hashing.
 * Each task carries a UID/GID set during login.
 *
 * Default accounts:
 *   root   (UID 0)   — superuser
 *   mandor (UID 1000) — default user
 */

#define MAX_USERS     8
#define USERNAME_MAX  32
#define SALT_SIZE     16
#define HASH_SIZE     32

/*
 * PBKDF2 iteration count — balance between security and bare-metal speed.
 *
 * BUG FIX: Previously 1000, which executes ~8000 SHA-256 transforms
 * during cred_init() (2 users × 1000 iterations × ~4 transforms each).
 * On bare metal, this blocks the boot path for seconds, causing
 * watchdog timeouts and/or boot_kernel_stack overflow (8KB stack
 * with ~750 bytes per nested call frame from PBKDF2→HMAC→SHA256).
 *
 * Reduced to 10 for boot-time initialization. Runtime authentication
 * (syscall 41) runs on per-task kernel stacks (16KB) with interrupts
 * enabled, so higher iterations can be used there if needed.
 */
#define PBKDF2_ITERATIONS 10

typedef struct {
    char     username[USERNAME_MAX];
    uint32_t uid;
    uint32_t gid;
    uint8_t  salt[SALT_SIZE];       /* Random salt for PBKDF2 */
    uint8_t  password_hash[HASH_SIZE]; /* PBKDF2-HMAC-SHA256 output */
    uint8_t  active;                /* 1 = account exists */
} user_account_t;

/* Initialize credential subsystem, create default accounts */
void cred_init(void);

/*
 * Authenticate user with username and password.
 * Returns UID (>= 0) on success, -1 on failure.
 */
int cred_authenticate(const char* username, const char* password);

/* Get username for a given UID; returns NULL if not found */
const char* cred_get_username(uint32_t uid);

/* Get UID for a given username; returns -1 if not found */
int cred_get_uid(const char* username);

/*
 * Change password for user with given UID.
 * Returns 0 on success, -1 on error.
 */
int cred_change_password(uint32_t uid, const char* new_password);

/*
 * Create a new user account.
 * Returns UID on success, -1 if table full or username taken.
 */
int cred_create_user(const char* username, const char* password, uint32_t gid);

/* Get total number of registered users */
int cred_user_count(void);

#include "random.h"
#include "chacha20.h"
#include "sha256.h"
#include "../kernel/utils.h"
#include "../cpu/spinlock.h"
#include "../drivers/serial.h"

/*
 * CSPRNG Implementation — ChaCha20-based with hardware seeding.
 *
 * Desain keamanan:
 *   - Seed awal dari RDRAND/RDSEED (jika tersedia) atau TSC + mixing
 *   - Output via ChaCha20 stream cipher (timing-safe)
 *   - Reseed otomatis setiap 1MB output
 *   - Entropy pool di-hash dengan SHA-256 sebelum digunakan sebagai key
 *   - Spinlock untuk thread-safety pada SMP
 */

#define RESEED_INTERVAL (1024 * 1024)  /* Reseed setiap 1MB output */
#define ENTROPY_POOL_SIZE 64           /* 512-bit entropy pool */

/* Internal state — protected by spinlock */
static chacha20_ctx_t rng_ctx;
static uint8_t entropy_pool[ENTROPY_POOL_SIZE];
static uint32_t pool_pos = 0;
static uint64_t bytes_since_reseed = 0;
static spinlock_t rng_lock = SPINLOCK_INIT;
static int rng_initialized = 0;

/*
 * Cek apakah CPU mendukung RDRAND (CPUID.01H:ECX bit 30)
 */
static int has_rdrand(void) {
    uint32_t ecx;
    asm volatile (
        "cpuid"
        : "=c"(ecx)
        : "a"(1)
        : "ebx", "edx"
    );
    return (ecx >> 30) & 1;
}

/*
 * Cek apakah CPU mendukung RDSEED (CPUID.07H:EBX bit 18)
 */
static int has_rdseed(void) {
    uint32_t ebx;
    asm volatile (
        "cpuid"
        : "=b"(ebx)
        : "a"(7), "c"(0)
        : "edx"
    );
    return (ebx >> 18) & 1;
}

/*
 * Baca 64-bit random dari RDRAND. Retry hingga 10 kali.
 * Return: 1 = sukses, 0 = gagal.
 */
static int rdrand64(uint64_t *val) {
    uint8_t ok;
    for (int i = 0; i < 10; i++) {
        asm volatile (
            "rdrand %0; setc %1"
            : "=r"(*val), "=qm"(ok)
        );
        if (ok) return 1;
    }
    return 0;
}

/*
 * Baca 64-bit random dari RDSEED. Retry hingga 10 kali.
 * Return: 1 = sukses, 0 = gagal.
 */
static int rdseed64(uint64_t *val) {
    uint8_t ok;
    for (int i = 0; i < 10; i++) {
        asm volatile (
            "rdseed %0; setc %1"
            : "=r"(*val), "=qm"(ok)
        );
        if (ok) return 1;
    }
    return 0;
}

/* Read Time Stamp Counter */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Mix entropy into pool using XOR at current position.
 * Pool wraps around when full.
 */
static void mix_into_pool(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        entropy_pool[pool_pos] ^= data[i];
        pool_pos = (pool_pos + 1) % ENTROPY_POOL_SIZE;
    }
}

/*
 * Gather hardware entropy dan seed ChaCha20 context.
 * Dipanggil saat init dan setiap reseed.
 */
static void seed_rng(void) {
    uint64_t hw_entropy[8]; /* 64 bytes of entropy */
    int hw_ok = 0;

    /* Coba RDSEED dulu (true entropy), lalu RDRAND, lalu TSC */
    if (has_rdseed()) {
        serial_write_string("[CRYPTO] Seeding from RDSEED (hardware true RNG)...\n");
        for (int i = 0; i < 8; i++) {
            if (!rdseed64(&hw_entropy[i])) {
                /* Fallback ke RDRAND jika RDSEED gagal */
                if (has_rdrand()) rdrand64(&hw_entropy[i]);
                else hw_entropy[i] = rdtsc();
            }
        }
        hw_ok = 1;
    } else if (has_rdrand()) {
        serial_write_string("[CRYPTO] Seeding from RDRAND (hardware PRNG)...\n");
        for (int i = 0; i < 8; i++) {
            if (!rdrand64(&hw_entropy[i])) {
                hw_entropy[i] = rdtsc();
            }
        }
        hw_ok = 1;
    }

    if (!hw_ok) {
        serial_write_string("[CRYPTO] WARNING: No RDRAND/RDSEED! Using TSC entropy.\n");
        for (int i = 0; i < 8; i++) {
            hw_entropy[i] = rdtsc();
            /* Busy-wait untuk jitter — setiap iterasi berbeda timing */
            for (volatile int j = 0; j < 100 + i * 37; j++) {
                asm volatile("pause");
            }
            hw_entropy[i] ^= rdtsc();
        }
    }

    /* Mix hardware entropy into pool */
    mix_into_pool((uint8_t*)hw_entropy, sizeof(hw_entropy));

    /* Hash pool → 32-byte key untuk ChaCha20 */
    uint8_t key[32];
    sha256_hash(entropy_pool, ENTROPY_POOL_SIZE, key);

    /* Generate nonce dari pool tail */
    uint8_t nonce[12];
    sha256_ctx_t nctx;
    sha256_init(&nctx);
    sha256_update(&nctx, entropy_pool, ENTROPY_POOL_SIZE);
    /* Add TSC untuk uniqueness */
    uint64_t tsc = rdtsc();
    sha256_update(&nctx, (uint8_t*)&tsc, 8);
    uint8_t nhash[32];
    sha256_final(&nctx, nhash);
    memcpy(nonce, nhash, 12);

    /* Reinitialize ChaCha20 dengan key dan nonce baru */
    chacha20_init(&rng_ctx, key, nonce, 0);
    bytes_since_reseed = 0;

    /* Wipe sensitive data */
    memset(key, 0, sizeof(key));
    memset(hw_entropy, 0, sizeof(hw_entropy));
    memset(nhash, 0, sizeof(nhash));
}

/* === Public API === */

void csprng_init(void) {
    serial_write_string("[INFO] Initializing CSPRNG...\n");

    memset(entropy_pool, 0, ENTROPY_POOL_SIZE);
    pool_pos = 0;

    seed_rng();
    rng_initialized = 1;

    serial_write_string("[OK] CSPRNG initialized and seeded!\n");
}

void csprng_get_bytes(uint8_t *buf, size_t len) {
    if (!rng_initialized) return;

    spin_lock(&rng_lock);

    /* Check if reseed needed */
    if (bytes_since_reseed > RESEED_INTERVAL) {
        seed_rng();
    }

    chacha20_keystream(&rng_ctx, buf, len);
    bytes_since_reseed += len;

    spin_unlock(&rng_lock);
}

uint64_t csprng_get_u64(void) {
    uint64_t val;
    csprng_get_bytes((uint8_t*)&val, sizeof(val));
    return val;
}

uint32_t csprng_get_u32(void) {
    uint32_t val;
    csprng_get_bytes((uint8_t*)&val, sizeof(val));
    return val;
}

void csprng_add_entropy(const uint8_t *data, size_t len) {
    if (!rng_initialized) return;

    spin_lock(&rng_lock);
    mix_into_pool(data, len);
    spin_unlock(&rng_lock);
}

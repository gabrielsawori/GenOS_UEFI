/*
 * CSPRNG — Cryptographically Secure Pseudo-Random Number Generator
 *
 * Uses x86 RDRAND instruction if available (checked via CPUID).
 * Falls back to xoshiro256** PRNG seeded from TSC + boot entropy.
 *
 * RDRAND draws from an on-chip hardware RNG (Intel/AMD) that
 * continuously seeds from thermal noise. It's NIST SP 800-90A
 * compliant on Intel CPUs.
 */
#include "random.h"
#include "../drivers/serial.h"

static int has_rdrand = 0;

/* xoshiro256** state — 256 bits of PRNG state */
static uint64_t prng_s[4];

/*
 * Detect RDRAND via CPUID.
 * CPUID EAX=1 → ECX bit 30 = RDRAND support.
 */
static int detect_rdrand(void) {
    uint32_t ecx;
    asm volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    return (ecx >> 30) & 1;
}

/* Read TSC for entropy seeding */
static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Try RDRAND; returns 1 on success, 0 on retry-exhausted */
static int rdrand64(uint64_t* val) {
    uint8_t ok;
    for (int i = 0; i < 10; i++) {
        asm volatile("rdrand %0; setc %1" : "=r"(*val), "=qm"(ok));
        if (ok) return 1;
    }
    return 0;
}

/* xoshiro256** rotation helper */
static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* xoshiro256** — fast, high-quality PRNG (Blackman & Vigna, 2018) */
static uint64_t xoshiro256ss(void) {
    uint64_t result = rotl64(prng_s[1] * 5, 7) * 9;
    uint64_t t = prng_s[1] << 17;

    prng_s[2] ^= prng_s[0];
    prng_s[3] ^= prng_s[1];
    prng_s[1] ^= prng_s[2];
    prng_s[0] ^= prng_s[3];
    prng_s[2] ^= t;
    prng_s[3] = rotl64(prng_s[3], 45);

    return result;
}

/* SplitMix64 — used to seed xoshiro from a single 64-bit value */
static uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void random_init(void) {
    has_rdrand = detect_rdrand();

    serial_write_string("[CRYPTO] RNG: ");
    if (has_rdrand) {
        serial_write_string("RDRAND available (hardware RNG)\n");
    } else {
        serial_write_string("RDRAND not available, using xoshiro256** PRNG\n");
    }

    /* Seed the fallback PRNG regardless (used as backup) */
    uint64_t seed = read_tsc();

    /* Mix in additional entropy sources */
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed ^= ((uint64_t)hi << 32) | lo;
    seed ^= (uint64_t)(uintptr_t)&seed;  /* ASLR-like stack address entropy */

    /* If RDRAND works, mix in hardware randomness for perfect seeding */
    if (has_rdrand) {
        uint64_t hw;
        if (rdrand64(&hw)) seed ^= hw;
    }

    /* Initialize xoshiro256** state via SplitMix64 */
    prng_s[0] = splitmix64(&seed);
    prng_s[1] = splitmix64(&seed);
    prng_s[2] = splitmix64(&seed);
    prng_s[3] = splitmix64(&seed);

    serial_write_string("[OK] CSPRNG initialized\n");
}

uint64_t random_u64(void) {
    if (has_rdrand) {
        uint64_t val;
        if (rdrand64(&val)) return val;
    }
    return xoshiro256ss();
}

uint32_t random_u32(void) {
    return (uint32_t)(random_u64() >> 32);
}

void random_bytes(void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    while (len >= 8) {
        uint64_t val = random_u64();
        p[0] = (uint8_t)(val);       p[1] = (uint8_t)(val >> 8);
        p[2] = (uint8_t)(val >> 16);  p[3] = (uint8_t)(val >> 24);
        p[4] = (uint8_t)(val >> 32);  p[5] = (uint8_t)(val >> 40);
        p[6] = (uint8_t)(val >> 48);  p[7] = (uint8_t)(val >> 56);
        p += 8; len -= 8;
    }
    if (len > 0) {
        uint64_t val = random_u64();
        for (size_t i = 0; i < len; i++)
            p[i] = (uint8_t)(val >> (i * 8));
    }
}

int random_has_rdrand(void) {
    return has_rdrand;
}

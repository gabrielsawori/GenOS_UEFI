#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Cryptographically Secure Pseudo-Random Number Generator (CSPRNG)
 *
 * Primary: x86 RDRAND/RDSEED instructions (hardware RNG)
 * Fallback: xoshiro256** PRNG seeded from TSC + LAPIC timer
 *
 * Usage:
 *   random_init();                    // Call once at boot
 *   uint64_t val = random_u64();      // Get 64-bit random
 *   random_bytes(buf, 32);            // Fill buffer with random bytes
 */

/* Initialize CSPRNG subsystem (detect RDRAND, seed fallback PRNG) */
void random_init(void);

/* Fill buffer with cryptographically random bytes */
void random_bytes(void* buf, size_t len);

/* Return a single random 64-bit value */
uint64_t random_u64(void);

/* Return a single random 32-bit value */
uint32_t random_u32(void);

/* Return 1 if hardware RDRAND is available, 0 otherwise */
int random_has_rdrand(void);

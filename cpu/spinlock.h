#pragma once
#include <stdint.h>

/*
 * Spinlock — SMP synchronization primitive.
 * Uses atomic test-and-set with PAUSE hint for efficient spinning.
 */
typedef volatile uint32_t spinlock_t;

#define SPINLOCK_INIT 0

static inline void spin_lock(spinlock_t* lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            asm volatile("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t* lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static inline int spin_trylock(spinlock_t* lock) {
    return !__atomic_test_and_set(lock, __ATOMIC_ACQUIRE);
}

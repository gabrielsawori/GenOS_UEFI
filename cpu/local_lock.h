#pragma once
#include "spinlock.h"
#include "preempt.h"
#ifdef GENOS_KERNEL
#include "tlb.h"
#endif

/* Subsystem lock preserving caller IRQ state and inhibiting local task
 * switches. Order: optional mutation guard -> ONE local lock -> IRQ leaf.
 * Never nest local locks, acquire the mutation guard, allocate or sleep here. */
struct local_lock_guard { spinlock_t* lock; };
static inline struct local_lock_guard local_lock_enter(spinlock_t* lock, int try_only) {
    if (preempt_local_depth()) __builtin_trap();
    preempt_local_disable();
    while (!spin_trylock(lock)) {
        if (try_only) {
            preempt_local_enable();
            return (struct local_lock_guard){0};
        }
#ifdef GENOS_KERNEL
        tlb_poll();
#endif
        asm volatile("pause");
    }
    return (struct local_lock_guard){lock};
}
static inline void local_lock_leave(struct local_lock_guard* guard) {
    if (!guard->lock) return;
    spin_unlock(guard->lock);
    preempt_local_enable();
}
#define LOCAL_LOCK_NAMED(lock, name, try_only) \
    struct local_lock_guard name __attribute__((cleanup(local_lock_leave), unused)) = \
        local_lock_enter(lock, try_only)
#define LOCAL_LOCK_GUARD(lock) LOCAL_LOCK_NAMED(lock, local_guard, 0)

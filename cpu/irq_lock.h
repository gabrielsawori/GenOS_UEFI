#pragma once
#include "spinlock.h"
#ifdef GENOS_KERNEL
#include "tlb.h"
#endif

/* Leaf lock: no allocation, mutation guard, sleeping or another lock while
 * held. Disable local IRQs before acquisition; a contended CPU services TLB
 * mailboxes so a remote shootdown does not depend on interrupt delivery.
 * A task may enter from a device lock; IRQ handlers never take device locks. */
struct irq_lock_guard { spinlock_t* lock; uint64_t flags; };

static inline struct irq_lock_guard irq_lock_enter(spinlock_t* lock) {
    struct irq_lock_guard guard = {.lock = lock, .flags = 0};
#ifdef GENOS_KERNEL
    asm volatile("pushfq; pop %0; cli" : "=r"(guard.flags) :: "memory");
#endif
    while (!spin_trylock(lock)) {
#ifdef GENOS_KERNEL
        tlb_poll();
#endif
        asm volatile("pause");
    }
    return guard;
}

static inline void irq_lock_leave(struct irq_lock_guard* guard) {
    spin_unlock(guard->lock);
#ifdef GENOS_KERNEL
    if (guard->flags & 512) asm volatile("sti" ::: "memory");
#endif
}

#define IRQ_LOCK_GUARD(lock) \
    struct irq_lock_guard irq_guard __attribute__((cleanup(irq_lock_leave), unused)) = \
        irq_lock_enter(lock)

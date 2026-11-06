#pragma once
#include <stdint.h>

/* Nested per-CPU preemption control plus a global kernel mutation lock.
 * IRQs stay enabled. Timer IRQs use try-acquire; other IRQs must not allocate
 * or acquire this lock. Never sleep with a guard held. */
void preempt_disable(void);
void preempt_enable(void);
uint32_t preempt_depth(void);
int preempt_try_disable(void);

/* Pin the running task without taking the global lock. IRQs stay enabled.
 * A local critical section must not acquire the mutation guard or sleep. */
void preempt_local_disable(void);
void preempt_local_enable(void);
uint32_t preempt_local_depth(void);

static inline void preempt_guard_leave(int* guard) {
    (void)guard;
    preempt_enable();
}

/* GCC cleanup balances the guard on every return, including error paths. */
#define PREEMPT_GUARD \
    int preempt_guard __attribute__((cleanup(preempt_guard_leave), unused)) = \
        (preempt_disable(), 0)

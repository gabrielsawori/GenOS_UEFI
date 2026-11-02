#include "preempt.h"
#ifdef GENOS_KERNEL
#include "percpu.h"
#include "spinlock.h"
#include "tlb.h"

/* Initial SMP policy: serialize kernel mutations, not userspace execution.
 * Timer IRQs try-acquire. Faults waiting with IF=0 service TLB mailboxes. */
static spinlock_t mutation_lock = SPINLOCK_INIT;
static uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static void irq_restore(uint64_t flags) {
    if (flags & 512) asm volatile("sti" ::: "memory");
}

void preempt_disable(void) {
    uint64_t flags = irq_save();
    struct percpu_data* p = percpu_current();
    uint32_t previous = p->preempt_count++;
    irq_restore(flags);
    if (!previous) {
        while (!spin_trylock(&mutation_lock)) {
            tlb_poll();
            asm volatile("pause");
        }
    }
}

int preempt_try_disable(void) {
    uint64_t flags = irq_save();
    struct percpu_data* p = percpu_current();
    int ok = !p->preempt_count && spin_trylock(&mutation_lock);
    if (ok) p->preempt_count = 1;
    irq_restore(flags);
    return ok;
}

void preempt_enable(void) {
    uint64_t flags = irq_save();
    struct percpu_data* p = percpu_current();
    if (!p->preempt_count) __builtin_trap();
    if (--p->preempt_count == 0) spin_unlock(&mutation_lock);
    irq_restore(flags);
}

uint32_t preempt_depth(void) { return percpu_current()->preempt_count; }
#else
/* Host allocator fixtures do not execute privileged instructions. */
static uint32_t depth;
void preempt_disable(void) { depth++; }
void preempt_enable(void) { if (!depth) __builtin_trap(); depth--; }
uint32_t preempt_depth(void) { return depth; }
int preempt_try_disable(void) { if (depth) return 0; depth = 1; return 1; }
#endif

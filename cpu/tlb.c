#include "tlb.h"
#include "percpu.h"
#include "lapic.h"
#include "../mm/vmm.h"

/* Serialized by the VMM lock. IRQ handler never acquires it. */
static uint64_t sequence;
static uint64_t address;

static void invalidate(uint64_t va) {
    if (va != UINT64_MAX) {
        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    } else {
        uint64_t cr4, cr3;
        asm volatile("mov %%cr4, %0; mov %%cr3, %1" : "=r"(cr4), "=r"(cr3));
        if (cr4 & (1ULL << 7)) {
            asm volatile("mov %0, %%cr4; mov %1, %%cr4" ::
                         "r"(cr4 & ~(1ULL << 7)), "r"(cr4) : "memory");
        } else asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
}

void tlb_poll(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    struct percpu_data* p = percpu_current();
    uint64_t request = __atomic_load_n(&p->tlb_request, __ATOMIC_ACQUIRE);
    if (request != __atomic_load_n(&p->tlb_ack, __ATOMIC_RELAXED)) {
        invalidate(address);
        __atomic_store_n(&p->tlb_ack, request, __ATOMIC_RELEASE);
    }
    if (flags & 512) asm volatile("sti" ::: "memory");
}

void tlb_shootdown(uint64_t* pml4, uint64_t va) {
    if (!vmm_lock_held()) __builtin_trap();
    (void)pml4;
    uint32_t self = smp_get_current_cpu_id();
    uint32_t targets = 0;
    uint64_t seq = ++sequence;
    address = va;
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
        struct percpu_data* p = percpu_get(i);
        if (i == self || !__atomic_load_n(&p->scheduler_online, __ATOMIC_ACQUIRE)) continue;
        /* Scheduler/CR3 changes no longer share our lock. Broadcast rather
         * than sampling active_cr3 and missing a CPU entering this root.
         * Context switches reload CR3; PCID is not enabled. */
        targets |= 1U << i;
        __atomic_store_n(&p->tlb_request, seq, __ATOMIC_RELEASE);
        lapic_send_ipi(p->lapic_id, LAPIC_TLB_VECTOR);
    }
    invalidate(va);
    for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
        if (!(targets & (1U << i))) continue;
        while (__atomic_load_n(&percpu_get(i)->tlb_ack, __ATOMIC_ACQUIRE) != seq) {
            tlb_poll();
            asm volatile("pause");
        }
    }
}

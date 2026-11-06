#pragma once
#include <stdint.h>
/* Internal VMM call: caller holds the VMM lock, never just PREEMPT_GUARD.
 * Broadcast to online CPUs; acknowledgments precede page/table reuse.
 * UINT64_MAX requests a full flush (including global translations). */
void tlb_shootdown(uint64_t* pml4, uint64_t va);
void tlb_poll(void);

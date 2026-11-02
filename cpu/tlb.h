#pragma once
#include <stdint.h>
/* Caller holds PREEMPT_GUARD. Acknowledgments precede page/table reuse.
 * UINT64_MAX requests a full flush (including global translations). */
void tlb_shootdown(uint64_t* pml4, uint64_t va);
void tlb_poll(void);

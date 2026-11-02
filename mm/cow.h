#pragma once
#include <stdint.h>

struct task;

/* Child is unpublished, has an empty PML4 and zeroed ownership records.
 * On failure the parent is unchanged; caller discards the partial child.
 * Caller holds PREEMPT_GUARD through publication/discard (SMP mutation lock).
 */
int cow_clone_address_space(struct task* parent, struct task* child);

/* Resolve a present write fault on a tracked private user page. Also supports
 * kernel writes to that user page (CR0.WP); never resolves RO/SHM/NX faults.
 * Returns 1 if repaired, 0 if not CoW or allocation failed.
 */
int cow_resolve(uint64_t fault_addr, uint64_t error_code);

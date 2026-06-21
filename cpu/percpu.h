#pragma once
#include <stdint.h>
#include "gdt.h"
#include "smp.h"

/*
 * Per-CPU Data — SMP support infrastructure.
 *
 * Each CPU has its own:
 *   - GDT (with unique TSS descriptor)
 *   - TSS (with unique RSP0 for Ring 3→0 transitions)
 *   - Kernel stack top (for syscall entry)
 *
 * This enables each CPU to independently handle interrupts from
 * Ring 3 user tasks, which is required for true SMP task execution.
 */

struct percpu_data {
    /* Identity */
    uint32_t cpu_id;           /* Index into cpus[] array */
    uint32_t lapic_id;         /* LAPIC ID */

    /* Per-CPU GDT + TSS */
    uint64_t  gdt[7];         /* Clone of BSP's GDT with per-CPU TSS */
    struct tss tss;            /* This CPU's TSS (RSP0 = kernel stack) */

    /* Stack management */
    uint64_t kernel_stack_top; /* Top of this CPU's current kernel stack */

    /* Alignment padding to cache line boundary */
    uint8_t  _pad[8];
} __attribute__((aligned(64))); /* Cache-line aligned to prevent false sharing */

/*
 * percpu_init_bsp() - Initialize per-CPU data for Bootstrap Processor.
 * Called once during boot after gdt_init().
 */
void percpu_init_bsp(void);

/*
 * percpu_init_ap() - Initialize per-CPU data for an Application Processor.
 * Creates per-AP GDT+TSS, loads GDT, loads TSS selector.
 * @param cpu_id:    Index into percpu array
 * @param lapic_id:  This AP's LAPIC ID
 * @param stack_top: Pre-allocated kernel stack top for this AP
 */
void percpu_init_ap(uint32_t cpu_id, uint32_t lapic_id, uint64_t stack_top);

/*
 * percpu_get() - Get per-CPU data for a specific CPU.
 * @param cpu_id: CPU index (0 = BSP)
 */
struct percpu_data* percpu_get(uint32_t cpu_id);

/*
 * percpu_set_kernel_stack() - Update kernel stack top for a CPU.
 * Called by scheduler after context switch to set TSS.RSP0.
 * @param cpu_id:    CPU index
 * @param stack_top: New kernel stack top
 */
void percpu_set_kernel_stack(uint32_t cpu_id, uint64_t stack_top);

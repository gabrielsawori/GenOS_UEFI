#include "percpu.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"
#include <stddef.h>

/*
 * Per-CPU Data Management
 *
 * Each CPU needs its own GDT+TSS because:
 *   1. TSS contains RSP0 (kernel stack for Ring 3→0 transitions)
 *   2. RSP0 must point to the CURRENT TASK's kernel stack on THAT CPU
 *   3. If all CPUs share one TSS, they'd all use the same RSP0 → crash
 *
 * Layout per CPU:
 *   percpu[N].gdt[0..6] = clone of BSP's GDT segments
 *   percpu[N].gdt[5..6] = TSS descriptor pointing to percpu[N].tss
 *   percpu[N].tss.rsp0  = current task's kernel stack on CPU N
 */

static struct percpu_data percpu[MAX_CPUS];

/* GDTR structure for loading per-CPU GDT */
struct percpu_gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * install_tss_in_gdt() — Encode a TSS descriptor into GDT slots 5-6.
 *
 * In x86-64, a TSS descriptor is 16 bytes (2 GDT slots):
 *   Slot 5: base[31:0], limit, type=0x89 (available TSS)
 *   Slot 6: base[63:32]
 */
static void install_tss_in_gdt(uint64_t* gdt, struct tss* tss_ptr) {
    uint64_t tss_base = (uint64_t)tss_ptr;
    uint32_t tss_limit = sizeof(struct tss) - 1;
    uint32_t base_low = tss_base & 0xFFFFFFFF;
    uint32_t base_high = tss_base >> 32;

    gdt[5] = (tss_limit & 0xFFFF) |
             ((uint64_t)(base_low & 0xFFFFFF) << 16) |
             (0x89ULL << 40) |
             (((uint64_t)((tss_limit >> 16) & 0xF)) << 48) |
             (((uint64_t)(base_low >> 24)) << 56);
    gdt[6] = base_high;
}

/*
 * percpu_init_bsp() — Set up per-CPU data for BSP (CPU 0).
 *
 * BSP already has its GDT+TSS from gdt_init(). We just record
 * metadata in percpu[0] for consistency. BSP continues using
 * its original GDT (no switch needed).
 */
void percpu_init_bsp(void) {
    percpu[0].cpu_id = 0;
    percpu[0].lapic_id = 0;
    percpu[0].kernel_stack_top = 0;

    serial_write_string("  [PERCPU] BSP (CPU #0) registered\n");
}

/*
 * percpu_init_ap() — Create per-CPU GDT+TSS for an Application Processor.
 *
 * Steps:
 *   1. Clone BSP's GDT entries (segments 0-4 are identical)
 *   2. Clear this AP's TSS
 *   3. Set RSP0 in TSS to the AP's kernel stack
 *   4. Encode TSS descriptor into slots 5-6 of this AP's GDT
 *   5. Load the new GDT on this AP
 *   6. Load the TSS selector (ltr 0x28)
 *
 * After this, the AP can safely handle Ring 3→0 transitions
 * because the CPU will read RSP0 from THIS AP's TSS.
 */
void percpu_init_ap(uint32_t cpu_id, uint32_t lapic_id, uint64_t stack_top) {
    if (cpu_id >= MAX_CPUS) return;

    struct percpu_data* p = &percpu[cpu_id];
    p->cpu_id = cpu_id;
    p->lapic_id = lapic_id;
    p->kernel_stack_top = stack_top;

    /* 1. Clone BSP's GDT segment descriptors (slots 0-4) */
    uint64_t* bsp_gdt = gdt_get_entries();
    for (int i = 0; i < 5; i++) {
        p->gdt[i] = bsp_gdt[i];
    }

    /* 2. Clear TSS */
    for (size_t i = 0; i < sizeof(struct tss); i++) {
        ((uint8_t*)&p->tss)[i] = 0;
    }
    p->tss.iopb_offset = sizeof(struct tss);
    p->tss.rsp0 = stack_top;

    /* 3. Install THIS AP's TSS into THIS AP's GDT */
    install_tss_in_gdt(p->gdt, &p->tss);

    /* 4. Load this AP's GDT */
    struct percpu_gdtr gdtr;
    gdtr.limit = sizeof(p->gdt) - 1;
    gdtr.base = (uint64_t)&p->gdt[0];
    gdt_load((uint64_t)&gdtr);

    /* 5. Load TSS selector (offset 0x28 = GDT slot 5) */
    asm volatile("ltr %w0" : : "r" ((uint16_t)0x28));
}

struct percpu_data* percpu_get(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return &percpu[0];
    return &percpu[cpu_id];
}

/*
 * percpu_set_kernel_stack() — Update RSP0 in a CPU's TSS.
 * Called by the scheduler after context switch.
 */
void percpu_set_kernel_stack(uint32_t cpu_id, uint64_t stack_top) {
    if (cpu_id >= MAX_CPUS) return;
    percpu[cpu_id].tss.rsp0 = stack_top;
    percpu[cpu_id].kernel_stack_top = stack_top;
}

void percpu_set_ist1(uint32_t cpu_id, uint64_t ist_stack) {
    if (cpu_id >= MAX_CPUS) return;
    percpu[cpu_id].tss.ist1 = ist_stack;
}

#pragma once
#include <stdint.h>

/* TSS (Task State Segment) x86_64 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0; /* Ring 3→0 stack pointer — THE critical field */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

void gdt_init(void);

/* Set kernel stack (RSP0) in the BSP's TSS */
void set_kernel_stack(uint64_t stack);

/* Load BSP's GDT on an AP (without TSS — use percpu for per-AP TSS) */
void gdt_load_on_ap(void);

/* Get BSP's GDT entries for cloning into per-CPU GDTs */
uint64_t* gdt_get_entries(void);
uint16_t  gdt_get_entry_count(void);

/* Assembly: load GDTR + reload segment registers */
extern void gdt_load(uint64_t gdtr_ptr);
#pragma once
#include <stdint.h>

void gdt_init(void);

// Fungsi untuk memberi tahu CPU di mana letak memori aman Kernel
void set_kernel_stack(uint64_t stack);

/*
 * gdt_load_on_ap() - Load BSP's GDT on an Application Processor.
 * APs share the BSP's GDT table but don't load TSS (they idle with cli;hlt).
 */
void gdt_load_on_ap(void);
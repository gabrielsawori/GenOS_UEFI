#pragma once
#include <stdint.h>

void gdt_init(void);

// Fungsi untuk memberi tahu CPU di mana letak memori aman Kernel
void set_kernel_stack(uint64_t stack);
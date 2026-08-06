#pragma once
#include <stdint.h>

void syscall_init(void);
void syscall_init_cpu(void);  /* Konfigurasi MSR syscall per-CPU (BSP + tiap AP) */
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3);
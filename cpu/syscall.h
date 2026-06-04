#pragma once
#include <stdint.h>

void syscall_init(void);
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3);
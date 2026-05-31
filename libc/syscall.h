#pragma once
#include <stdint.h>

// Fungsi ajaib untuk membungkus perintah Assembly "syscall" ke dalam bahasa C
static inline uint64_t syscall(uint64_t sys_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    asm volatile (
        "syscall"
        : "=a" (ret)
        : "a" (sys_num), "D" (arg1), "S" (arg2), "d" (arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}
#pragma once
#include <stdint.h>

// Struktur ini mewakili keadaan (state) CPU tepat saat error terjadi
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8; // Register umum
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;    // Register umum
    uint64_t int_no, err_code;                     // Nomor Error & Kode Error
    uint64_t rip, cs, rflags, rsp, ss;             // Status dari CPU otomatis
} __attribute__((packed));

void isr_handler(struct registers *regs);
#pragma once
#include <stdint.h>

/*
 * struct registers — CPU state snapshot saat interrupt/exception.
 *
 * Layout ini HARUS 100% identik dengan stack frame yang dibentuk oleh
 * isr_common di isr_asm.S. Urutan field = push order (offset rendah ke tinggi):
 *
 *   [RSP+0x00] R15    ← terakhir di-push = offset terendah
 *   [RSP+0x08] R14
 *   [RSP+0x10] R13
 *   [RSP+0x18] R12
 *   [RSP+0x20] R11
 *   [RSP+0x28] R10
 *   [RSP+0x30] R9
 *   [RSP+0x38] R8
 *   [RSP+0x40] RDI
 *   [RSP+0x48] RSI
 *   [RSP+0x50] RBP
 *   [RSP+0x58] RBX
 *   [RSP+0x60] RDX
 *   [RSP+0x68] RCX
 *   [RSP+0x70] RAX
 *   [RSP+0x78] vector     (push $i oleh ISR stub)
 *   [RSP+0x80] err_code   (push $0 atau CPU push)
 *   [RSP+0x88] RIP        ← CPU-pushed iret frame
 *   [RSP+0x90] CS
 *   [RSP+0x98] RFLAGS
 *   [RSP+0xA0] RSP
 *   [RSP+0xA8] SS
 *
 * Total: 22 × uint64_t = 176 bytes
 */
struct registers {
    /* General-purpose registers (di-push oleh isr_common) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

    /* Interrupt identification (di-push oleh ISR stub macro) */
    uint64_t vector;     /* Nomor interrupt / exception (sebelumnya int_no) */
    uint64_t err_code;   /* Error code (0 jika exception tanpa error code) */

    /* CPU-pushed iret frame (otomatis oleh CPU saat interrupt) */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

void isr_handler(struct registers *regs);
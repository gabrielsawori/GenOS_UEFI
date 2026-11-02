#pragma once
#include <stdint.h>

/* FXSAVE64 layout; kernel-owned and 16-byte aligned on every task. Only
 * x87/MMX/SSE are enabled: AVX/XSAVE state is not part of this ABI yet. */
struct fpu_state {
    uint16_t control, status;
    uint8_t tag, reserved;
    uint16_t opcode;
    uint64_t instruction, operand;
    uint32_t mxcsr, mxcsr_mask;
    uint8_t registers[480];
} __attribute__((aligned(16)));

void fpu_init_cpu(void);
void fpu_state_init(struct fpu_state* state);
void fpu_save(struct fpu_state* state);
void fpu_restore(const struct fpu_state* state);

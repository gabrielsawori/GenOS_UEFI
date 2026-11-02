#include "fpu.h"
#include "../drivers/serial.h"
#include <stddef.h>

_Static_assert(sizeof(struct fpu_state) == 512, "FXSAVE area size");
_Static_assert(offsetof(struct fpu_state, mxcsr) == 24, "FXSAVE MXCSR offset");

/* Construct an empty x87 stack and zero ALL register payloads. FNINIT alone
 * leaves old register bytes observable through FXSAVE in a new process. */
static const struct fpu_state initial = { .control = 0x037f, .mxcsr = 0x1f80 };

void fpu_state_init(struct fpu_state* state) { *state = initial; }

void fpu_save(struct fpu_state* state) {
    asm volatile("fxsave64 %0" : "=m"(*state) :: "memory");
}

void fpu_restore(const struct fpu_state* state) {
    asm volatile("fxrstor64 %0" :: "m"(*state) : "memory");
}

void fpu_init_cpu(void) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    const uint32_t required = (1U << 0) | (1U << 24) | (1U << 25) | (1U << 26);
    if ((d & required) != required) {
        serial_write_string("[FATAL] x87/FXSAVE/SSE2 required\n");
        for (;;) asm volatile("cli; hlt");
    }
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    cr0 = (cr0 | (1ULL << 1) | (1ULL << 5)) & ~((1ULL << 2) | (1ULL << 3));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR, OSXMMEXCPT */
    cr4 &= ~(1ULL << 18); /* No OSXSAVE: do not expose unsaved AVX state. */
    asm volatile("mov %0, %%cr0; mov %1, %%cr4" :: "r"(cr0), "r"(cr4) : "memory");
    fpu_restore(&initial);
}

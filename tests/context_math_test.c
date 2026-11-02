#include "../libc/syscall.h"

/* Build this TU with production USER_CFLAGS. The register-probe TU uses
 * integer-only code so compiler-generated SIMD cannot overwrite its probes. */
int context_math_test(void) {
    volatile double increment = 1.5;
    double value = increment;
    for (unsigned i = 0; i < 64; i++) {
        value += increment;
        syscall(4, 1, 0, 0);
    }
    return value == 97.5;
}

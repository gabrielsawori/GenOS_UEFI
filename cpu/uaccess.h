#pragma once
#include <stdint.h>
#include <stddef.h>

#define USER_ADDR_MAX 0x800000000000ULL
#define USER_STRING_MAX 4096

/* Arithmetic check only; use user_access_ok for mapping/permission checks. */
static inline int is_user_range(uint64_t addr, size_t len) {
    return !len || (addr && addr < USER_ADDR_MAX && len <= USER_ADDR_MAX - addr);
}

/* Current address space, 4 KiB user mappings only. A CoW leaf is accepted
 * for write access; ordinary RO and supervisor mappings are not.
 * Hold PREEMPT_GUARD across validation and subsequent use of a pointer.
 */
int user_access_ok(const void* ptr, size_t len, int write);
/* Resolve CoW before an operation with side effects (e.g. advancing an FD).
 * Returns 0 or -1. The caller holds a guard through the operation and copy.
 */
int user_write_prepare(void* ptr, size_t len);

/* Return 0 on success, -1 on invalid mapping/permission or copy fault/OOM.
 * Zero length succeeds without dereferencing either pointer. Each copy holds
 * its own BSP preemption guard. On fault/OOM a prefix may have been copied;
 * callers must not consume a failed copy_from_user destination.
 */
int copy_from_user(void* kernel_dst, const void* user_src, size_t len);
int copy_to_user(void* user_dst, const void* kernel_src, size_t len);
/* Copy at most capacity bytes including NUL. Return length excluding NUL,
 * or -1 for an invalid/unterminated string. Never read beyond its terminator.
 */
int copy_user_string(char* kernel_dst, const char* user_src, size_t capacity);

struct registers;
/* Only the annotated user load/store instructions can recover a kernel #PF. */
int uaccess_fixup(struct registers* regs, uint64_t fault_addr);

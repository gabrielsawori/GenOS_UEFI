#include "uaccess.h"
#include "isr.h"
#include "preempt.h"
#include "../kernel/task.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../mm/cow.h"

/* Private assembly primitives: callers validate user permissions first. */
extern int uaccess_copy_from_asm(void*, const void*, size_t);
extern int uaccess_copy_to_asm(void*, const void*, size_t);
struct copy_fixup { uint64_t instruction, recovery, write; };
extern const struct copy_fixup uaccess_fixups_start[], uaccess_fixups_end[];

int user_access_ok(const void* ptr, size_t len, int write) {
    PREEMPT_GUARD;
    uint64_t addr = (uint64_t)ptr;
    if (!is_user_range(addr, len)) return 0;
    if (!len) return 1;
    uint64_t* root = task_get_current_pml4();
    uint64_t end = addr + len;
    for (uint64_t va = addr & ~(PAGE_SIZE - 1ULL); va < end; va += PAGE_SIZE) {
        if (!vmm_user_accessible(root, va, write)) return 0;
    }
    return 1;
}

int user_write_prepare(void* ptr, size_t len) {
    PREEMPT_GUARD;
    if (!user_access_ok(ptr, len, 1)) return -1;
    if (!len) return 0;
    uint64_t end = (uint64_t)ptr + len;
    for (uint64_t va = (uint64_t)ptr & ~(PAGE_SIZE - 1ULL); va < end; va += PAGE_SIZE) {
        uint64_t entry = vmm_read_pte_in(task_get_current_pml4(), va);
        if (!entry || (!(entry & PTE_WRITE) && !cow_resolve(va, 3))) return -1;
    }
    return 0;
}

int copy_from_user(void* dst, const void* src, size_t len) {
    PREEMPT_GUARD;
    if (!user_access_ok(src, len, 0)) return -1;
    return len ? uaccess_copy_from_asm(dst, src, len) : 0;
}

int copy_to_user(void* dst, const void* src, size_t len) {
    PREEMPT_GUARD;
    if (!user_access_ok(dst, len, 1)) return -1;
    return len ? uaccess_copy_to_asm(dst, src, len) : 0;
}

int copy_user_string(char* dst, const char* src, size_t capacity) {
    PREEMPT_GUARD;
    uint64_t addr = (uint64_t)src;
    if (!capacity || capacity > USER_STRING_MAX || !is_user_range(addr, 1)) return -1;
    for (size_t i = 0; i < capacity; i++) {
        if (!i || !((addr + i) & (PAGE_SIZE - 1))) {
            if (!user_access_ok((const void*)(addr + i), 1, 0)) return -1;
        }
        if (uaccess_copy_from_asm(dst + i, (const void*)(addr + i), 1)) return -1;
        if (!dst[i]) return (int)i;
    }
    return -1;
}

int uaccess_fixup(struct registers* regs, uint64_t addr) {
    if ((regs->cs & 3) || regs->vector != 14 || !preempt_depth() ||
        !is_user_range(addr, 1) || (regs->err_code & ~3ULL)) return 0;
    for (const struct copy_fixup* f = uaccess_fixups_start; f < uaccess_fixups_end; f++) {
        if (regs->rip != f->instruction || !!(regs->err_code & 2) != f->write) continue;
        if (addr != (f->write ? regs->rdi : regs->rsi)) return 0;
        regs->rip = f->recovery;
        return 1;
    }
    return 0;
}

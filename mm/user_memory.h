#pragma once
#include <stdint.h>
#include <stddef.h>

struct task;
#define USER_INLINE_PAGES 32U
#define USER_MAX_PAGES 65536U /* Policy limit: 256 MiB of private resident pages. */
#define USER_HEAP_BASE 0x60000000ULL
#define USER_HEAP_END  0x68000000ULL
#define USER_SHM_BASE  0x90000000ULL
#define USER_SHM_END   0xd0000000ULL
#define USER_STACK_BASE 0x80001000ULL
#define USER_STACK_PAGES 4U

/* Metadata is kernel-owned; user code cannot forge allocation headers. */
struct user_allocation {
    uint64_t base;
    size_t size;
    uint32_t pages;
    struct user_allocation* next;
};

void user_memory_init(struct task* t);
int user_pages_reserve(struct task* t, uint32_t count);
void user_memory_release(struct task* t);
int user_allocations_clone(struct task* parent, struct task* child);
/* First fit, including one unmapped trailing guard page. Caller holds guard. */
uint64_t user_find_free_range(struct task* owner, uint64_t start, uint64_t end,
                             uint32_t pages);
uint64_t user_alloc(struct task* t, size_t size);
int user_free(struct task* t, uint64_t base);
uint64_t user_realloc(struct task* t, uint64_t base, size_t size);

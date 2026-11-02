#include "user_memory.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../cpu/preempt.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../limine.h"

extern volatile struct limine_hhdm_request hhdm_request;

void user_memory_init(struct task* t) {
    t->user_pages = t->inline_pages;
    t->user_vaddrs = t->inline_vaddrs;
    t->user_page_capacity = USER_INLINE_PAGES;
}

int user_pages_reserve(struct task* t, uint32_t count) {
    PREEMPT_GUARD;
    if (!t || t->is_thread || count > USER_MAX_PAGES) return 0;
    if (count <= t->user_page_capacity) return 1;
    uint32_t capacity = t->user_page_capacity;
    while (capacity < count) capacity *= 2;
    uint64_t* data = kmalloc((size_t)capacity * 2 * sizeof(uint64_t));
    if (!data) return 0;
    memcpy(data, t->user_pages, t->user_page_count * sizeof(uint64_t));
    memcpy(data + capacity, t->user_vaddrs, t->user_page_count * sizeof(uint64_t));
    if (t->user_pages != t->inline_pages) kfree(t->user_pages);
    t->user_pages = data;
    t->user_vaddrs = data + capacity;
    t->user_page_capacity = capacity;
    return 1;
}

/* Only for an inactive, unpublished or reaped address-space owner. */
void user_memory_release(struct task* t) {
    PREEMPT_GUARD;
    if (t->is_thread) return;
    for (uint32_t i = 0; i < t->user_page_count; i++)
        pmm_free_page((void*)t->user_pages[i]);
    if (t->user_pages != t->inline_pages) kfree(t->user_pages);
    while (t->allocations) {
        struct user_allocation* next = t->allocations->next;
        kfree(t->allocations);
        t->allocations = next;
    }
    t->user_page_count = 0;
    user_memory_init(t);
}

int user_allocations_clone(struct task* parent, struct task* child) {
    PREEMPT_GUARD;
    if (child->allocations) return 0;
    struct user_allocation** tail = &child->allocations;
    for (struct user_allocation* a = parent->allocations; a; a = a->next) {
        struct user_allocation* copy = kmalloc(sizeof(*copy));
        if (!copy) return 0; /* Constructor discards partial metadata. */
        *copy = *a;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    return 1;
}

uint64_t user_find_free_range(struct task* t, uint64_t start, uint64_t end,
                             uint32_t pages) {
    uint64_t span = ((uint64_t)pages + 1) * PAGE_SIZE;
    if (!t || !t->pml4 || !pages || start >= end || end > 0x800000000000ULL ||
        (start & (PAGE_SIZE - 1)) || span > end - start) return 0;
    uint64_t candidate = start;
    while (candidate <= end - span) {
        uint64_t skip = candidate;
        for (struct user_allocation* a = t->allocations; a; a = a->next) {
            uint64_t stop = a->base + ((uint64_t)a->pages + 1) * PAGE_SIZE;
            if (candidate < stop && a->base < candidate + span && stop > skip) skip = stop;
        }
        for (unsigned i = 0; i < SHM_MAX_ATTACH; i++) {
            shm_attach_record_t* a = &t->shm_attached[i];
            uint64_t stop = a->vaddr + ((uint64_t)a->page_count + 1) * PAGE_SIZE;
            if (a->shmid >= 0 && candidate < stop && a->vaddr < candidate + span &&
                stop > skip) skip = stop;
        }
        if (skip != candidate) { candidate = skip; continue; }
        /* ELF mappings can occupy these ranges; never overwrite them or place
         * a guard on an already mapped page. */
        for (uint64_t va = candidate; va < candidate + span; va += PAGE_SIZE) {
            if (vmm_pte_in(t->pml4, va)) { skip = va + PAGE_SIZE; break; }
        }
        if (skip == candidate) return candidate;
        candidate = skip;
    }
    return 0;
}

uint64_t user_alloc(struct task* task, size_t size) {
    PREEMPT_GUARD;
    struct task* t = task_memory_owner(task);
    if (!t || !t->pml4 || !size || size > USER_HEAP_END - USER_HEAP_BASE - PAGE_SIZE)
        return 0;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > USER_MAX_PAGES - t->user_page_count) return 0;
    uint64_t base = user_find_free_range(t, USER_HEAP_BASE, USER_HEAP_END, pages);
    if (!base) return 0;
    struct user_allocation* a = kmalloc(sizeof(*a));
    if (!a) return 0;
    if (!user_pages_reserve(t, t->user_page_count + pages)) { kfree(a); return 0; }
    uint32_t before = t->user_page_count;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) goto rollback;
        memset((void*)(phys + hhdm_request.response->offset), 0, PAGE_SIZE);
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        uint32_t index = t->user_page_count++;
        t->user_pages[index] = phys;
        t->user_vaddrs[index] = va;
    }
    if (!vmm_map_pages_in(t->pml4, base, t->user_pages + before, pages, VMM_FLAGS_USER_DATA))
        goto rollback;
    *a = (struct user_allocation){.base = base, .size = size, .pages = pages,
                                  .next = t->allocations};
    t->allocations = a;
    return base;
rollback:
    while (t->user_page_count > before) {
        uint32_t index = --t->user_page_count;
        /* No mappings published on frame OOM; range mapping rolls itself back. */
        pmm_free_page((void*)t->user_pages[index]);
    }
    kfree(a);
    return 0;
}

int user_free(struct task* task, uint64_t base) {
    PREEMPT_GUARD;
    if (!base) return 0;
    struct task* t = task_memory_owner(task);
    if (!t || !t->pml4) return -1;
    struct user_allocation** link = &t->allocations;
    while (*link && (*link)->base != base) link = &(*link)->next;
    if (!*link) return -1; /* Exact base only; never accept a kernel/ELF/SHM pointer. */
    struct user_allocation* a = *link;
    uint64_t end = base + (uint64_t)a->pages * PAGE_SIZE;
    vmm_unmap_pages_in(t->pml4, base, a->pages);
    for (uint32_t i = 0; i < t->user_page_count;) {
        uint64_t va = t->user_vaddrs[i];
        if (va < base || va >= end) { i++; continue; }
        /* Range removal has acknowledged every remote TLB before frame reuse. */
        pmm_free_page((void*)t->user_pages[i]);
        uint32_t last = --t->user_page_count;
        t->user_pages[i] = t->user_pages[last];
        t->user_vaddrs[i] = t->user_vaddrs[last];
    }
    *link = a->next;
    kfree(a);
    return 0;
}

uint64_t user_realloc(struct task* task, uint64_t base, size_t size) {
    PREEMPT_GUARD;
    if (!base) return user_alloc(task, size);
    struct task* t = task_memory_owner(task);
    if (!t || !t->pml4) return 0;
    struct user_allocation* a = t->allocations;
    while (a && a->base != base) a = a->next;
    if (!a) return 0;
    if (!size) { user_free(t, base); return 0; }
    if (size == a->size) return base;
    uint64_t replacement = user_alloc(t, size);
    if (!replacement) return 0; /* Old block remains valid on failure. */
    size_t bytes = size < a->size ? size : a->size;
    for (size_t offset = 0; offset < bytes; offset += PAGE_SIZE) {
        uint64_t src = vmm_get_physical_in(t->pml4, base + offset, NULL);
        uint64_t dst = vmm_get_physical_in(t->pml4, replacement + offset, NULL);
        size_t n = bytes - offset < PAGE_SIZE ? bytes - offset : PAGE_SIZE;
        memcpy((void*)(dst + hhdm_request.response->offset),
               (void*)(src + hhdm_request.response->offset), n);
    }
    user_free(t, base);
    return replacement;
}

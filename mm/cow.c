#include "cow.h"
#include "pmm.h"
#include "vmm.h"
#include "shm.h"
#include "../cpu/preempt.h"
#include "../cpu/tlb.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../limine.h"

extern volatile struct limine_hhdm_request hhdm_request;

static int private_index(struct task* owner, uint64_t va, uint64_t phys) {
    for (uint32_t i = 0; i < owner->user_page_count; i++) {
        if (owner->user_vaddrs[i] == va && owner->user_pages[i] == phys)
            return (int)i;
    }
    return -1;
}

struct clone_context {
    struct task* owner;
    struct task* child;
};

static int clone_page(uint64_t va, uint64_t entry, void* context) {
    struct clone_context* c = context;
    uint64_t phys = vmm_pte_address(entry);
    uint64_t flags = entry & PTE_FLAG_MASK;
    if (!(flags & PTE_USER)) return 0;
    if (private_index(c->owner, va, phys) < 0) {
        /* Untracked/device mappings need an explicit inheritance policy. */
        if ((flags & PTE_COW) || !shm_owns_mapping(c->owner, va, phys)) return 0;
        return vmm_map_page_in(c->child->pml4, va, phys, flags);
    }
    if (c->child->user_page_count >= c->child->user_page_capacity ||
        !pmm_ref_page((void*)phys)) return 0;
    if (flags & (PTE_WRITE | PTE_COW)) flags = (flags & ~PTE_WRITE) | PTE_COW;
    if (!vmm_map_page_in(c->child->pml4, va, phys, flags)) {
        pmm_free_page((void*)phys);
        return 0;
    }
    uint32_t index = c->child->user_page_count++;
    c->child->user_pages[index] = phys;
    c->child->user_vaddrs[index] = va;
    return 1;
}

int cow_clone_address_space(struct task* parent, struct task* child) {
    PREEMPT_GUARD;
    struct task* owner = task_memory_owner(parent);
    if (!owner || !owner->pml4 || owner->user_page_count > USER_MAX_PAGES ||
        !child || !child->pml4 || child->user_page_count) return 0;
    if (!user_pages_reserve(child, owner->user_page_count) ||
        !user_allocations_clone(owner, child)) return 0;
    struct clone_context context = {owner, child};
    if (!vmm_visit_user_pages(owner->pml4, clone_page, &context) ||
        child->user_page_count != owner->user_page_count ||
        !shm_inherit_attachments(owner, child)) return 0;

    /* Commit only after all fallible work. Failed forks leave every parent
     * PTE and attachment count unchanged; task_discard releases child refs.
     */
    for (uint32_t i = 0; i < child->user_page_count; i++) {
        uint64_t va = child->user_vaddrs[i];
        uint64_t* parent_pte = vmm_pte_in(owner->pml4, va);
        if (*parent_pte & (PTE_WRITE | PTE_COW)) {
            *parent_pte = (*parent_pte & ~PTE_WRITE) | PTE_COW;
            tlb_shootdown(owner->pml4, va);
        }
    }
    return 1;
}

int cow_resolve(uint64_t fault_addr, uint64_t error_code) {
    PREEMPT_GUARD;
    /* Only present write faults, with optional user bit. Reserved-bit,
     * instruction-fetch, protection-key and other faults are not CoW. */
    if ((error_code & ~4ULL) != 3 || fault_addr >= 0x800000000000ULL) return 0;
    struct task* owner = task_memory_owner(task_find_by_pid(get_current_pid()));
    if (!owner || !owner->pml4) return 0;
    uint64_t va = fault_addr & ~(PAGE_SIZE - 1ULL);
    uint64_t* pte = vmm_pte_in(owner->pml4, va);
    if (!pte) return 0;
    /* A sibling may have resolved the same write fault while we waited for
     * the mutation lock. Retry with the now-writable translation. */
    if ((*pte & (PTE_USER | PTE_WRITE)) == (PTE_USER | PTE_WRITE)) {
        tlb_shootdown(owner->pml4, va);
        return 1;
    }
    if ((*pte & (PTE_USER | PTE_COW | PTE_WRITE)) != (PTE_USER | PTE_COW)) return 0;
    uint64_t old_phys = vmm_pte_address(*pte);
    int index = private_index(owner, va, old_phys);
    uint32_t refs = pmm_get_refcount((void*)old_phys);
    if (index < 0 || !refs) return 0;
    uint64_t flags = ((*pte & PTE_FLAG_MASK) & ~PTE_COW) | PTE_WRITE;
    if (refs == 1) {
        *pte = old_phys | flags;
    } else {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) return 0;
        uint64_t offset = hhdm_request.response->offset;
        memcpy((void*)(phys + offset), (void*)(old_phys + offset), PAGE_SIZE);
        *pte = phys | flags;
        /* Replace this VA's ownership instead of appending a second record. */
        owner->user_pages[index] = phys;
        tlb_shootdown(owner->pml4, va);
        pmm_free_page((void*)old_phys);
        return 1;
    }
    tlb_shootdown(owner->pml4, va);
    return 1;
}

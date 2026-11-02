#include "vmm.h"
#include "pmm.h"
#include "../cpu/preempt.h"
#include "../cpu/percpu.h"
#include "../cpu/tlb.h"
#include "../limine.h"
#include "../drivers/serial.h"

extern volatile struct limine_hhdm_request hhdm_request;
static uint64_t* kernel_pml4;

static uint64_t* table_at(uint64_t entry) {
    return (uint64_t*)(vmm_pte_address(entry) + hhdm_request.response->offset);
}

static int canonical(uint64_t va) {
    return va < 0x800000000000ULL || va >= 0xFFFF800000000000ULL;
}

static unsigned index_at(uint64_t va, unsigned level) {
    return (va >> (12 + 9 * level)) & 511;
}

void vmm_init(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = table_at(cr3);
    /* Kernel stores to user CoW mappings must fault instead of bypassing RO. */
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 16;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    serial_write_string("[OK] VMM linked to Limine page tables\n");
}

static int map_page(uint64_t* pml4, uint64_t va, uint64_t phys, uint64_t flags,
                    int flush) {
    if (!pml4 || !canonical(va) || (va & (PAGE_SIZE - 1)) ||
        (phys & ~PTE_ADDR_MASK) || !(flags & PTE_PRESENT) ||
        (flags & ~PTE_FLAG_MASK) || ((flags & PTE_USER) && va >= 0x800000000000ULL))
        return 0;
    uint64_t* table = pml4;
    uint64_t* created[3];
    unsigned count = 0;
    for (unsigned level = 3; level > 0; level--) {
        uint64_t* slot = &table[index_at(va, level)];
        if (*slot & PTE_PRESENT) {
            /* Do not widen ancestor permissions and expose other mappings. */
            if ((*slot & PTE_LARGE) ||
                ((flags & PTE_USER) && !(*slot & PTE_USER)) ||
                ((flags & PTE_WRITE) && !(*slot & PTE_WRITE)) ||
                (!(flags & PTE_NX) && (*slot & PTE_NX))) goto rollback;
        } else {
            uint64_t frame = (uint64_t)pmm_alloc_page();
            if (!frame) goto rollback;
            uint64_t* next = table_at(frame);
            for (unsigned i = 0; i < 512; i++) next[i] = 0;
            *slot = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
            created[count++] = slot;
        }
        table = table_at(*slot);
    }
    uint64_t* leaf = &table[index_at(va, 0)];
    if (*leaf & PTE_PRESENT) goto rollback;
    *leaf = phys | flags;
    if (flush) tlb_shootdown(pml4, va);
    return 1;
rollback:
    {
        uint64_t retired[3];
        unsigned n = 0;
        while (count) {
            uint64_t* slot = created[--count];
            retired[n++] = vmm_pte_address(*slot);
            *slot = 0;
        }
        if (n) tlb_shootdown(pml4, UINT64_MAX);
        while (n) pmm_free_page((void*)retired[--n]);
    }
    return 0;
}

int vmm_map_page_in(uint64_t* pml4, uint64_t va, uint64_t phys, uint64_t flags) {
    PREEMPT_GUARD;
    return map_page(pml4, va, phys, flags, 1);
}

static int valid_range(uint64_t base, uint32_t count) {
    uint64_t bytes = (uint64_t)count * PAGE_SIZE;
    return count && !(base & (PAGE_SIZE - 1)) && canonical(base) &&
           bytes - 1 <= UINT64_MAX - base && canonical(base + bytes - 1);
}

int vmm_map_pages_in(uint64_t* pml4, uint64_t base, const uint64_t* pages,
                     uint32_t count, uint64_t flags) {
    PREEMPT_GUARD;
    if (!pml4 || !pages || !valid_range(base, count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!map_page(pml4, base + (uint64_t)i * PAGE_SIZE, pages[i], flags, 0)) {
            if (i) vmm_unmap_pages_in(pml4, base, i);
            return 0;
        }
    }
    tlb_shootdown(pml4, UINT64_MAX);
    return 1;
}

int vmm_map_page(uint64_t va, uint64_t phys, uint64_t flags) {
    return vmm_map_page_in(kernel_pml4, va, phys, flags);
}

uint64_t* vmm_pte_in(uint64_t* pml4, uint64_t va) {
    if (!pml4 || !canonical(va)) return NULL;
    uint64_t* table = pml4;
    for (unsigned level = 3; level > 0; level--) {
        uint64_t entry = table[index_at(va, level)];
        if (!(entry & PTE_PRESENT) || (entry & PTE_LARGE)) return NULL;
        table = table_at(entry);
    }
    uint64_t* leaf = &table[index_at(va, 0)];
    return (*leaf & PTE_PRESENT) ? leaf : NULL;
}

uint64_t vmm_get_physical_in(uint64_t* pml4, uint64_t va, uint64_t* flags) {
    PREEMPT_GUARD;
    uint64_t* entry = vmm_pte_in(pml4, va);
    if (flags) *flags = entry ? (*entry & PTE_FLAG_MASK) : 0;
    return entry ? vmm_pte_address(*entry) : 0;
}

uint64_t vmm_get_physical(uint64_t va, uint64_t* flags) {
    return vmm_get_physical_in(kernel_pml4, va, flags);
}

int vmm_user_accessible(uint64_t* pml4, uint64_t va, int write) {
    PREEMPT_GUARD;
    if (!pml4 || va >= 0x800000000000ULL) return 0;
    uint64_t* table = pml4;
    for (int level = 3; level >= 0; level--) {
        uint64_t entry = table[index_at(va, (unsigned)level)];
        if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return 0;
        if (level) {
            if ((entry & PTE_LARGE) || (write && !(entry & PTE_WRITE))) return 0;
            table = table_at(entry);
        } else if (write && !(entry & (PTE_WRITE | PTE_COW))) return 0;
    }
    return 1;
}

static int empty_table(uint64_t* table) {
    for (unsigned i = 0; i < 512; i++) {
        if (table[i] & PTE_PRESENT) return 0;
    }
    return 1;
}

/* A bounded retirement buffer keeps stack usage small. Even when a large
 * range fills it, no table frame is recycled before remote walkers flush. */
static void release_tables(uint64_t* pml4, uint64_t* retired, unsigned* count) {
    tlb_shootdown(pml4, UINT64_MAX);
    while (*count) pmm_free_page((void*)retired[--*count]);
}

uint32_t vmm_unmap_pages_in(uint64_t* pml4, uint64_t base, uint32_t count) {
    PREEMPT_GUARD;
    if (!pml4 || !valid_range(base, count)) return 0;
    uint32_t removed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t* leaf = vmm_pte_in(pml4, base + (uint64_t)i * PAGE_SIZE);
        if (leaf) { *leaf = 0; removed++; }
    }
    if (!removed) return 0;
    uint64_t retired[16];
    unsigned n = 0;
    /* Inspect each affected PT once, including a partially covered PT. */
    uint64_t va = base;
    uint64_t last = base + ((uint64_t)count - 1) * PAGE_SIZE;
    for (;;) {
        uint64_t* tables[4] = {pml4};
        uint64_t* slots[3];
        unsigned depth;
        for (depth = 0; depth < 3; depth++) {
            slots[depth] = &tables[depth][index_at(va, 3 - depth)];
            if (!(*slots[depth] & PTE_PRESENT) || (*slots[depth] & PTE_LARGE)) break;
            tables[depth + 1] = table_at(*slots[depth]);
        }
        if (depth == 3) {
            for (int i = 2; i >= 0; i--) {
                if (i == 0 && va >= 0xFFFF800000000000ULL) break;
                if (!empty_table(tables[i + 1])) break;
                retired[n++] = vmm_pte_address(*slots[i]);
                *slots[i] = 0;
                if (n == 16) release_tables(pml4, retired, &n);
            }
        }
        if ((va >> 21) == (last >> 21)) break;
        va = (va | ((1ULL << 21) - 1)) + 1;
    }
    release_tables(pml4, retired, &n);
    return removed;
}

uint64_t vmm_unmap_page_in(uint64_t* pml4, uint64_t va) {
    PREEMPT_GUARD;
    if (!pml4 || !canonical(va) || (va & (PAGE_SIZE - 1))) return 0;
    uint64_t* tables[4] = {pml4};
    uint64_t* slots[3];
    for (unsigned i = 0; i < 3; i++) {
        slots[i] = &tables[i][index_at(va, 3 - i)];
        if (!(*slots[i] & PTE_PRESENT) || (*slots[i] & PTE_LARGE)) return 0;
        tables[i + 1] = table_at(*slots[i]);
    }
    uint64_t* leaf = &tables[3][index_at(va, 0)];
    if (!(*leaf & PTE_PRESENT)) return 0;
    uint64_t phys = vmm_pte_address(*leaf);
    *leaf = 0;
    uint64_t retired[3];
    unsigned n = 0;
    for (int i = 2; i >= 0; i--) {
        /* Upper-half roots are copied into every process; keep their PDPT. */
        if (i == 0 && va >= 0xFFFF800000000000ULL) break;
        if (!empty_table(tables[i + 1])) break;
        uint64_t frame = vmm_pte_address(*slots[i]);
        *slots[i] = 0;
        retired[n++] = frame;
    }
    tlb_shootdown(pml4, n ? UINT64_MAX : va);
    while (n) pmm_free_page((void*)retired[--n]);
    return phys;
}

uint64_t vmm_unmap_page(uint64_t va) {
    return vmm_unmap_page_in(kernel_pml4, va);
}

uint64_t* vmm_create_address_space(void) {
    PREEMPT_GUARD;
    uint64_t phys = (uint64_t)pmm_alloc_page();
    if (!phys) return NULL;
    uint64_t* pml4 = table_at(phys);
    for (unsigned i = 0; i < 512; i++) pml4[i] = i < 256 ? 0 : kernel_pml4[i];
    return pml4;
}

/* Free table frames only; leaf references belong to the caller. Huge leaves
 * are skipped, never dereferenced or freed as though they were page tables. */
static void free_tables(uint64_t* table, unsigned level, unsigned limit) {
    if (level) {
        for (unsigned i = 0; i < limit; i++) {
            uint64_t entry = table[i];
            if (!(entry & PTE_PRESENT) || (entry & PTE_LARGE)) continue;
            free_tables(table_at(entry), level - 1, 512);
        }
    }
    pmm_free_page((void*)((uint64_t)table - hhdm_request.response->offset));
}

void vmm_destroy_address_space(uint64_t* pml4) {
    PREEMPT_GUARD;
    if (pml4 && pml4 != kernel_pml4) free_tables(pml4, 3, 256);
}

static int visit_table(uint64_t* table, unsigned level, unsigned limit,
                       uint64_t base, vmm_visit_fn visit, void* context) {
    for (unsigned i = 0; i < limit; i++) {
        uint64_t entry = table[i];
        if (!(entry & PTE_PRESENT)) continue;
        uint64_t va = base | ((uint64_t)i << (12 + 9 * level));
        if (level) {
            if ((entry & PTE_LARGE) ||
                !visit_table(table_at(entry), level - 1, 512, va, visit, context))
                return 0;
        } else if (!visit(va, entry, context)) return 0;
    }
    return 1;
}

int vmm_visit_user_pages(uint64_t* pml4, vmm_visit_fn visit, void* context) {
    PREEMPT_GUARD;
    return pml4 && visit && visit_table(pml4, 3, 256, 0, visit, context);
}

void vmm_switch_pml4(uint64_t* pml4) {
    if (!pml4) pml4 = kernel_pml4;
    uint64_t phys = (uint64_t)pml4 - hhdm_request.response->offset;
    uint64_t old;
    asm volatile("mov %%cr3, %0" : "=r"(old));
    if (old != phys) asm volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
    percpu_current()->active_cr3 = phys;
}

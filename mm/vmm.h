#pragma once
#include <stdint.h>

void vmm_init(void);

/* Page Table Entry flags */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_LARGE    (1ULL << 7)   /* Huge leaf at PD/PDPT levels */
#define PTE_COW      (1ULL << 9)
#define PTE_NX       (1ULL << 63)  /* No-Execute: page cannot contain code */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_FLAG_MASK (0xFFFULL | PTE_NX)

static inline uint64_t vmm_pte_address(uint64_t entry) {
    return entry & PTE_ADDR_MASK;
}

/* Common flag combinations */
#define VMM_FLAGS_KERNEL     (PTE_PRESENT | PTE_WRITE)           /* 0x03 */
#define VMM_FLAGS_USER_RO    (PTE_PRESENT | PTE_USER)            /* 0x05 */
#define VMM_FLAGS_USER_RW    (PTE_PRESENT | PTE_WRITE | PTE_USER) /* 0x07 */
#define VMM_FLAGS_USER_CODE  (PTE_PRESENT | PTE_USER)            /* 0x05, executable */
#define VMM_FLAGS_USER_DATA  (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) /* writable, NOT executable */
#define VMM_FLAGS_USER_STACK (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) /* writable, NOT executable */

// === Operasi pada kernel PML4 (default, backward-compatible) ===

// Fungsi Sakti untuk membuat Portal dari Alamat Palsu (Virtual) ke Alamat Asli (Fisik)
int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Melepas Portal: menghapus mapping virtual→fisik untuk 1 halaman
uint64_t vmm_unmap_page(uint64_t virtual_addr);

// Menerjemahkan Alamat Palsu → Alamat Asli tanpa mengubah apapun
uint64_t vmm_get_physical(uint64_t virtual_addr, uint64_t* out_flags);

// === Operasi pada PML4 tertentu (per-process address space) ===

// Membuat address space baru: alokasi PML4 + salin upper half dari kernel
uint64_t* vmm_create_address_space(void);

// Menghancurkan address space: bebaskan page table level user (lower half)
void vmm_destroy_address_space(uint64_t* pml4);

// Petakan halaman ke PML4 tertentu
/* Return 1 on success, 0 on invalid address, collision, huge leaf or OOM.
 * Never overwrite a mapping. Failed calls free new page tables. Mapping and
 * unmapping do not acquire/release the backing page reference.
 */
int vmm_map_page_in(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

/* Transactional range mapping: publish all leaves then invalidate once.
 * Failure removes only leaves created by this call; backing references remain
 * owned by the caller. Input physical-page array must stay stable under guard.
 */
int vmm_map_pages_in(uint64_t* pml4, uint64_t base, const uint64_t* pages,
                     uint32_t count, uint64_t flags);
/* Clear a range and prune empty tables, waiting for remote TLBs before freeing
 * table frames. Returns removed leaf count; caller can then release backing
 * references. Missing/huge leaves are skipped; no partial huge-page unmapping.
 */
uint32_t vmm_unmap_pages_in(uint64_t* pml4, uint64_t base, uint32_t count);

/* 4 KiB leaves only. Hold PREEMPT_GUARD while using a returned PTE pointer. */
uint64_t* vmm_pte_in(uint64_t* pml4, uint64_t virtual_addr);
/* Effective USER/WRITE permissions through all levels. CoW can satisfy
 * write access at the leaf only. Huge mappings are currently unsupported. */
int vmm_user_accessible(uint64_t* pml4, uint64_t va, int write);
uint64_t vmm_get_physical_in(uint64_t* pml4, uint64_t virtual_addr, uint64_t* flags);
typedef int (*vmm_visit_fn)(uint64_t va, uint64_t entry, void* context);
/* Callback must not mutate the traversed tree. Huge mappings are rejected. */
int vmm_visit_user_pages(uint64_t* pml4, vmm_visit_fn visit, void* context);

// Lepas mapping dari PML4 tertentu
uint64_t vmm_unmap_page_in(uint64_t* pml4, uint64_t virtual_addr);

// Ganti CR3 ke PML4 tertentu; NULL memilih kernel PML4 (idle/reaper).
void vmm_switch_pml4(uint64_t* pml4);

#include "vmm.h"
#include "pmm.h"
#include "../limine.h"
#include "../drivers/serial.h"

// --- PERBAIKAN: Jangan buat request baru! Pinjam (extern) dari pmm.c ---
extern volatile struct limine_hhdm_request hhdm_request;

// Pointer ke Tabel Lapis Ke-4 (Puncak dari Pohon Paging)
static uint64_t* kernel_pml4 = NULL;

void vmm_init(void) {
    serial_write_string("[INFO] Menginisialisasi Virtual Memory Manager (VMM)...\n");

    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    
    cr3 &= ~0xFFF; 
    
    // Gunakan hhdm_request dari pmm.c
    kernel_pml4 = (uint64_t*)(cr3 + hhdm_request.response->offset);
    
    serial_write_string("[OK] Mesin Paging (VMM) berhasil terkait dengan tabel Limine!\n");
}

/* ========================================================================
 * INTERNAL: Operasi page table pada PML4 tertentu
 * Semua fungsi publik hanyalah wrapper yang memanggil versi _in dengan
 * kernel_pml4 sebagai parameter default.
 * ======================================================================== */

void vmm_map_page_in(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t offset = hhdm_request.response->offset;
    
    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    // --- Lapis 1: PDPT ---
    uint64_t* pdpt;
    if (pml4[pml4_idx] & 1) { 
        pdpt = (uint64_t*)((pml4[pml4_idx] & ~0xFFF) + offset);
    } else { 
        uint64_t p = (uint64_t)pmm_alloc_page();
        pdpt = (uint64_t*)(p + offset);
        for(int i=0; i<512; i++) pdpt[i] = 0; 
        pml4[pml4_idx] = p | 0b111;    
    }

    // --- Lapis 2: PD ---
    uint64_t* pd;
    if (pdpt[pdpt_idx] & 1) {
        pd = (uint64_t*)((pdpt[pdpt_idx] & ~0xFFF) + offset);
    } else {
        uint64_t p = (uint64_t)pmm_alloc_page();
        pd = (uint64_t*)(p + offset);
        for(int i=0; i<512; i++) pd[i] = 0;
        pdpt[pdpt_idx] = p | 0b111;
    }

    // --- Lapis 3: PT ---
    uint64_t* pt;
    if (pd[pd_idx] & 1) {
        pt = (uint64_t*)((pd[pd_idx] & ~0xFFF) + offset);
    } else {
        uint64_t p = (uint64_t)pmm_alloc_page();
        pt = (uint64_t*)(p + offset);
        for(int i=0; i<512; i++) pt[i] = 0;
        pd[pd_idx] = p | 0b111;
    }

    /* --- Lapis Terakhir: Tautkan dengan Fisik Asli --- */
    pt[pt_idx] = physical_addr | flags;

    /* FLUSH TLB untuk alamat virtual ini. */
    asm volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    vmm_map_page_in(kernel_pml4, virtual_addr, physical_addr, flags);
}

uint64_t vmm_unmap_page_in(uint64_t* pml4, uint64_t virtual_addr) {
    uint64_t offset = hhdm_request.response->offset;

    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)((pml4[pml4_idx] & ~0xFFF) + offset);

    if (!(pdpt[pdpt_idx] & 1)) return 0;
    uint64_t* pd = (uint64_t*)((pdpt[pdpt_idx] & ~0xFFF) + offset);

    if (!(pd[pd_idx] & 1)) return 0;
    uint64_t* pt = (uint64_t*)((pd[pd_idx] & ~0xFFF) + offset);

    if (!(pt[pt_idx] & 1)) return 0;

    uint64_t phys = pt[pt_idx] & ~0xFFF;
    pt[pt_idx] = 0;

    asm volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return phys;
}

uint64_t vmm_unmap_page(uint64_t virtual_addr) {
    return vmm_unmap_page_in(kernel_pml4, virtual_addr);
}

uint64_t vmm_get_physical(uint64_t virtual_addr, uint64_t* out_flags) {
    uint64_t offset = hhdm_request.response->offset;

    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    if (!(kernel_pml4[pml4_idx] & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)((kernel_pml4[pml4_idx] & ~0xFFF) + offset);

    if (!(pdpt[pdpt_idx] & 1)) return 0;
    uint64_t* pd = (uint64_t*)((pdpt[pdpt_idx] & ~0xFFF) + offset);

    if (!(pd[pd_idx] & 1)) return 0;
    uint64_t* pt = (uint64_t*)((pd[pd_idx] & ~0xFFF) + offset);

    uint64_t entry = pt[pt_idx];
    if (!(entry & 1)) return 0;

    if (out_flags) {
        *out_flags = entry & 0xFFF;
    }
    return entry & ~0xFFF;
}

/* ========================================================================
 * PER-PROCESS ADDRESS SPACE MANAGEMENT
 * ======================================================================== */

/*
 * vmm_create_address_space: Alokasi PML4 baru untuk proses user.
 *
 * Mengalokasikan 1 page fisik untuk PML4, meng-nol-kan seluruhnya,
 * lalu menyalin entries 256-511 (upper half) dari kernel_pml4.
 * Upper half di-SHARE (pointer yang sama), bukan di-copy deep,
 * sehingga kernel mapping selalu identik di semua address space.
 *
 * Return: pointer virtual (HHDM) ke PML4 baru, atau NULL jika gagal.
 */
uint64_t* vmm_create_address_space(void) {
    uint64_t offset = hhdm_request.response->offset;

    uint64_t phys_pml4 = (uint64_t)pmm_alloc_page();
    if (!phys_pml4) return NULL;

    uint64_t* new_pml4 = (uint64_t*)(phys_pml4 + offset);

    /* Nol-kan seluruh PML4 */
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;

    /* Salin upper half (entries 256-511) dari kernel — SHARED, bukan deep copy */
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    return new_pml4;
}

/*
 * vmm_destroy_address_space: Bebaskan page table levels user (lower half).
 *
 * Walk entries 0-255 dari PML4, bebaskan setiap PT, PD, dan PDPT page
 * yang dialokasikan untuk lower half. TIDAK membebaskan physical leaf
 * pages — itu tanggung jawab reaper (via user_pages[]).
 * TIDAK membebaskan upper half karena di-share dengan kernel.
 */
void vmm_destroy_address_space(uint64_t* pml4) {
    if (!pml4) return;

    uint64_t offset = hhdm_request.response->offset;

    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & 1)) continue;

        uint64_t* pdpt = (uint64_t*)((pml4[i] & ~0xFFF) + offset);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & 1)) continue;

            uint64_t* pd = (uint64_t*)((pdpt[j] & ~0xFFF) + offset);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & 1)) continue;

                /* Bebas PT page */
                uint64_t pt_phys = pd[k] & ~0xFFF;
                pmm_free_page((void*)pt_phys);
            }
            /* Bebas PD page */
            uint64_t pd_phys = pdpt[j] & ~0xFFF;
            pmm_free_page((void*)pd_phys);
        }
        /* Bebas PDPT page */
        uint64_t pdpt_phys = pml4[i] & ~0xFFF;
        pmm_free_page((void*)pdpt_phys);
    }

    /* Bebas PML4 page itu sendiri */
    uint64_t pml4_phys = (uint64_t)pml4 - offset;
    pmm_free_page((void*)pml4_phys);
}

/*
 * vmm_switch_pml4: Ganti address space aktif dengan menulis CR3.
 *
 * @param pml4: pointer virtual (HHDM) ke PML4. Fungsi ini menghitung
 *              alamat fisik dan menulisnya ke CR3.
 */
void vmm_switch_pml4(uint64_t* pml4) {
    uint64_t offset = hhdm_request.response->offset;
    uint64_t phys = (uint64_t)pml4 - offset;
    asm volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}

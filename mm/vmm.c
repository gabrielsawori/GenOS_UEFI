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

void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    // Gunakan hhdm_request dari pmm.c
    uint64_t offset = hhdm_request.response->offset;
    
    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    // --- Lapis 1: PDPT ---
    uint64_t* pdpt;
    if (kernel_pml4[pml4_idx] & 1) { 
        pdpt = (uint64_t*)((kernel_pml4[pml4_idx] & ~0xFFF) + offset);
    } else { 
        uint64_t p = (uint64_t)pmm_alloc_page();
        pdpt = (uint64_t*)(p + offset);
        for(int i=0; i<512; i++) pdpt[i] = 0; 
        kernel_pml4[pml4_idx] = p | 0b111;    
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

    // --- Lapis Terakhir: Tautkan dengan Fisik Asli ---
    pt[pt_idx] = physical_addr | flags;
}
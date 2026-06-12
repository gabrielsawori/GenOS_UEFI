#pragma once
#include <stdint.h>

void vmm_init(void);

// === Operasi pada kernel PML4 (default, backward-compatible) ===

// Fungsi Sakti untuk membuat Portal dari Alamat Palsu (Virtual) ke Alamat Asli (Fisik)
void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

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
void vmm_map_page_in(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Lepas mapping dari PML4 tertentu
uint64_t vmm_unmap_page_in(uint64_t* pml4, uint64_t virtual_addr);

// Ganti CR3 ke PML4 tertentu (switch address space)
void vmm_switch_pml4(uint64_t* pml4);
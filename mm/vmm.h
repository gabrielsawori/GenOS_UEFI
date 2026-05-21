#pragma once
#include <stdint.h>

void vmm_init(void);

// Fungsi Sakti untuk membuat Portal dari Alamat Palsu (Virtual) ke Alamat Asli (Fisik)
void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
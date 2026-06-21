#pragma once
#include <stdint.h>
#include <stddef.h>

// 1 Page (Petak) berukuran 4 Kilobytes
#define PAGE_SIZE 4096

void pmm_init(void);
void* pmm_alloc_page(void);       // Fungsi untuk meminta 1 petak RAM kosong
void pmm_free_page(void* ptr);    // Fungsi untuk mengembalikan petak RAM ke sistem
uint64_t pmm_get_free_ram(void);  // Melihat sisa RAM kosong

/* === Copy-on-Write (CoW) Reference Counting === */
void pmm_ref_page(void* ptr);         // Tambah reference count (untuk CoW sharing)
uint8_t pmm_get_refcount(void* ptr);  // Cek reference count (>1 = shared/CoW)
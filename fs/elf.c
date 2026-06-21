#include "elf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../kernel/utils.h"
#include "../limine.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Pinjam hhdm_request dari pmm.c untuk akses fisik langsung */
extern volatile struct limine_hhdm_request hhdm_request;

/* ELF program header flags */
#define PF_X 0x1  /* Execute */
#define PF_W 0x2  /* Write */
#define PF_R 0x4  /* Read */

/*
 * elf_load: Muat ELF ke address space proses tertentu.
 *
 * @param binary_data:  pointer ke data biner ELF (di kernel space)
 * @param target_pml4:  PML4 tujuan (address space proses baru)
 * @param phys_pages:   array untuk mencatat page fisik yang dialokasikan
 * @param page_count:   pointer ke counter page (di-update oleh fungsi ini)
 *
 * Untuk setiap segmen PT_LOAD:
 *   1. Alokasi page fisik via PMM
 *   2. Tulis data ELF ke page fisik via HHDM (kernel bisa akses semua fisik)
 *   3. Petakan page fisik ke target_pml4 di alamat virtual user
 *   4. Catat page fisik di phys_pages[] untuk cleanup nanti
 *
 * Proteksi memori berdasarkan ELF flags:
 *   - Code (PF_R|PF_X): Present + User (read-only, executable)  = 0x05
 *   - Data (PF_R|PF_W): Present + User + Write                  = 0x07
 *
 * Return: entry point ELF, atau 0 jika gagal.
 */
uint64_t elf_load(uint8_t* binary_data, uint64_t* target_pml4,
                  uint64_t* phys_pages, uint32_t* page_count) {
    elf_header_t* header = (elf_header_t*)binary_data;

    // Check Magic Number
    if (header->magic != ELF_MAGIC) {
        return 0;
    }

    uint64_t hhdm_offset = hhdm_request.response->offset;
    elf_phdr_t* phdr = (elf_phdr_t*)(binary_data + header->phoff);

    for (int i = 0; i < header->phnum; i++) {
        if (phdr[i].type == 1) { /* PT_LOAD */
            uint64_t vaddr = phdr[i].vaddr;
            uint64_t memsz = phdr[i].memsz;
            uint64_t filesz = phdr[i].filesz;
            uint64_t offset = phdr[i].offset;

            /* Tentukan flags berdasarkan ELF segment flags */
            uint64_t page_flags = 0x01 | 0x04; /* Present + User */
            if (phdr[i].flags & PF_W) {
                page_flags |= 0x02; /* Writable */
            }

            /* Alokasi dan petakan page fisik untuk segmen ini */
            for (uint64_t j = 0; j < memsz; j += PAGE_SIZE) {
                void* phys_page = pmm_alloc_page();
                if (!phys_page) return 0;

                /* Catat page fisik untuk cleanup nanti */
                if (phys_pages && page_count && *page_count < 256) {
                    phys_pages[*page_count] = (uint64_t)phys_page;
                    (*page_count)++;
                }

                /* Nol-kan page fisik dulu (untuk BSS dan alignment padding) */
                uint8_t* phys_virt = (uint8_t*)((uint64_t)phys_page + hhdm_offset);
                for (int k = 0; k < PAGE_SIZE; k++) phys_virt[k] = 0;

                /* Hitung berapa byte yang perlu di-copy dari file ke page ini */
                uint64_t seg_offset = j;          /* Offset dalam segmen */
                uint64_t copy_len = 0;
                if (seg_offset < filesz) {
                    copy_len = filesz - seg_offset;
                    if (copy_len > PAGE_SIZE) copy_len = PAGE_SIZE;
                }

                /* Copy data ELF ke page fisik via HHDM */
                if (copy_len > 0) {
                    uint8_t* src = binary_data + offset + seg_offset;
                    for (uint64_t k = 0; k < copy_len; k++) {
                        phys_virt[k] = src[k];
                    }
                }

                /* Petakan ke address space target */
                vmm_map_page_in(target_pml4, vaddr + j, (uint64_t)phys_page, page_flags);
            }
        }
    }

    return header->entry;
}

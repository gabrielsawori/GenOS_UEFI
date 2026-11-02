#include "elf.h"
#include "elf_validate.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../kernel/utils.h"
#include "../limine.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Pinjam hhdm_request dari pmm.c untuk akses fisik langsung */
extern volatile struct limine_hhdm_request hhdm_request;

/*
 * elf_load: Muat ELF ke address space proses tertentu.
 *
 * @param binary_data:  pointer ke data biner ELF (di kernel space)
 * @param binary_size:  ukuran buffer biner yang dapat dibaca
 * @param target_pml4:  PML4 tujuan (address space proses baru)
 * @param phys_pages:   array untuk mencatat page fisik yang dialokasikan
 * @param virtual_pages: VA setiap page fisik untuk penggantian ownership CoW
 * @param page_count:   pointer ke counter page (di-update oleh fungsi ini)
 * @param page_capacity: kapasitas tracking yang boleh dipakai loader
 *
 * Untuk setiap segmen PT_LOAD:
 *   1. Alokasi page fisik via PMM
 *   2. Catat ownership untuk cleanup, termasuk jika mapping berikutnya gagal
 *   3. Nol-kan page dan tulis data ELF via HHDM
 *   4. Petakan page fisik ke target_pml4 di alamat virtual user
 *
 * Proteksi memori berdasarkan ELF flags:
 *   - Code (PF_R|PF_X): Present + User (read-only, executable)  = 0x05
 *   - Data (PF_R|PF_W): Present + User + Write + NX
 *   - Read-only data: Present + User + NX
 *
 * Return: entry point ELF, atau 0 jika gagal.
 */
uint64_t elf_load(const uint8_t* binary_data, size_t binary_size, uint64_t* target_pml4,
                  uint64_t* phys_pages, uint64_t* virtual_pages,
                  uint32_t* page_count, uint32_t page_capacity) {
    elf_image_info_t image;
    if (!target_pml4 || !phys_pages || !virtual_pages || !page_count ||
        *page_count > page_capacity ||
        !elf_validate(binary_data, binary_size, page_capacity - *page_count, &image)) return 0;

    uint64_t hhdm_offset = hhdm_request.response->offset;
    for (unsigned i = 0; i < image.header.phnum; i++) {
        elf_phdr_t segment;
        memcpy(&segment, binary_data + image.header.phoff + i * sizeof(segment), sizeof(segment));
        if (segment.type == ELF_PT_LOAD) {
            uint64_t vaddr = segment.vaddr;
            uint64_t memsz = segment.memsz;
            uint64_t filesz = segment.filesz;
            uint64_t offset = segment.offset;
            uint64_t page_flags = PTE_PRESENT | PTE_USER;
            if (segment.flags & ELF_PF_W) page_flags |= PTE_WRITE;
            if (!(segment.flags & ELF_PF_X)) page_flags |= PTE_NX;

            /* Alokasi dan petakan page fisik untuk segmen ini */
            uint64_t first_page = vaddr & ~(PAGE_SIZE - 1ULL);
            uint64_t end = vaddr + memsz;
            for (uint64_t page_va = first_page; memsz && page_va < end; page_va += PAGE_SIZE) {
                void* phys_page = pmm_alloc_page();
                if (!phys_page) return 0;

                /* Record ownership before mapping, including failed mappings. */
                phys_pages[*page_count] = (uint64_t)phys_page;
                virtual_pages[*page_count] = page_va;
                (*page_count)++;

                /* Nol-kan page fisik dulu (untuk BSS dan alignment padding) */
                uint8_t* phys_virt = (uint8_t*)((uint64_t)phys_page + hhdm_offset);
                for (int k = 0; k < PAGE_SIZE; k++) phys_virt[k] = 0;

                /* PT_LOAD may start within a page. Copy only its file-backed
                 * intersection; zero prefix/suffix and BSS stay untouched. */
                uint64_t copy_start = page_va < vaddr ? vaddr : page_va;
                uint64_t copy_end = page_va + PAGE_SIZE;
                if (copy_end > vaddr + filesz) copy_end = vaddr + filesz;
                if (copy_end > copy_start) {
                    const uint8_t* src = binary_data + offset + (copy_start - vaddr);
                    for (uint64_t k = 0; k < copy_end - copy_start; k++) {
                        phys_virt[copy_start - page_va + k] = src[k];
                    }
                }

                /* Petakan ke address space target */
                if (!vmm_map_page_in(target_pml4, page_va,
                                     (uint64_t)phys_page, page_flags)) return 0;
            }
        }
    }

    return image.header.entry;
}

#include "elf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../kernel/utils.h"

uint64_t elf_load(uint8_t* binary_data) {
    elf_header_t* header = (elf_header_t*)binary_data;

    // Check Magic Number
    if (header->magic != ELF_MAGIC) {
        return 0; // Not a valid ELF
    }

    elf_phdr_t* phdr = (elf_phdr_t*)(binary_data + header->phoff);

    for (int i = 0; i < header->phnum; i++) {
        if (phdr[i].type == 1) { // PT_LOAD
            uint64_t vaddr = phdr[i].vaddr;
            uint64_t memsz = phdr[i].memsz;
            uint64_t filesz = phdr[i].filesz;
            uint64_t offset = phdr[i].offset;

            // Allocate memory and map it
            for (uint64_t j = 0; j < memsz; j += PAGE_SIZE) {
                void* phys_page = pmm_alloc_page();
                vmm_map_page((uint64_t)phys_page, vaddr + j, 0x07); // User, RW, Present
            }

            // Copy data to mapped memory
            memcpy((void*)vaddr, binary_data + offset, filesz);

            // BSS: Zero out remaining memory
            if (memsz > filesz) {
                memset((void*)(vaddr + filesz), 0, memsz - filesz);
            }
        }
    }

    return header->entry;
}

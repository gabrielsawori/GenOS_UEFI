#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

#define ELF_MAGIC 0x464C457F
#define ELF_PT_LOAD 1
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

typedef struct {
    uint32_t magic;
    uint8_t  elf[12];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf_header_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf_phdr_t;

/* Kernel-owned binary must remain readable and unchanged for binary_size bytes.
 * Returns entry or 0. Failed allocations/mappings leave ownership records for
 * the caller to discard, along with its unpublished address space.
 */
uint64_t elf_load(const uint8_t* binary_data, size_t binary_size, uint64_t* target_pml4,
                  uint64_t* phys_pages, uint64_t* virtual_pages,
                  uint32_t* page_count, uint32_t page_capacity);

#endif

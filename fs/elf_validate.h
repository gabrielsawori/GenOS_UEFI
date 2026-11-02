#pragma once
#include "elf.h"

#define ELF_MAX_PROGRAM_HEADERS 128

typedef struct {
    elf_header_t header;
    uint32_t page_count;
} elf_image_info_t;

/* Validate the supported static ELF64/x86-64 subset without allocating or
 * modifying anything. max_pages is the caller's remaining image-page budget.
 * Return 1 or 0; optional info is only written on success. Buffer is trusted
 * kernel memory of the supplied size, but every byte in it is untrusted data.
 */
int elf_validate(const uint8_t* binary, size_t size, uint32_t max_pages,
                 elf_image_info_t* info);

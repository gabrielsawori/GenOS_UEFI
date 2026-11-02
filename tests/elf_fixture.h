#pragma once
#include "../fs/elf_validate.h"

typedef struct {
    elf_header_t header;
    elf_phdr_t segments[3];
    uint8_t payload[32];
} test_elf_t;

static inline test_elf_t test_elf_valid(void) {
    test_elf_t image = {
        .header = {.magic = ELF_MAGIC, .elf = {2, 1, 1}, .type = 2, .machine = 62,
            .version = 1, .entry = 0x40000000 + offsetof(test_elf_t, payload),
            .phoff = sizeof(elf_header_t), .ehsize = sizeof(elf_header_t),
            .phentsize = sizeof(elf_phdr_t), .phnum = 1},
        .segments = {{.type = ELF_PT_LOAD, .flags = ELF_PF_R | ELF_PF_X,
            .offset = offsetof(test_elf_t, payload),
            .vaddr = 0x40000000 + offsetof(test_elf_t, payload),
            .filesz = 32, .memsz = 4096 - offsetof(test_elf_t, payload), .align = 4096}}
    };
    for (unsigned i = 0; i < sizeof(image.payload); i++) image.payload[i] = (uint8_t)(0x80 + i);
    return image;
}

#define TEST_ELF_INVALID_CASES 44
/* Return the declared buffer length. Each case is malformed/unsupported, or
 * exceeds the caller's image-page budget. No allocations needed. */
static inline size_t test_elf_invalid(test_elf_t* image, unsigned which, uint32_t budget) {
    *image = test_elf_valid();
    elf_header_t* h = &image->header;
    elf_phdr_t* p = &image->segments[0];
    switch (which) {
    case 0: return 0;
    case 1: return sizeof(*h) - 1;
    case 2: h->magic = 0; break;
    case 3: h->elf[0] = 1; break;
    case 4: h->elf[1] = 2; break;
    case 5: h->elf[2] = 0; break;
    case 6: h->elf[3] = 255; break;
    case 7: h->elf[4] = 1; break;
    case 8: h->type = 3; break;
    case 9: h->machine = 3; break;
    case 10: h->version = 0; break;
    case 11: h->flags = 1; break;
    case 12: h->ehsize--; break;
    case 13: h->phentsize--; break;
    case 14: h->phnum = 0; break;
    case 15: h->phnum = 0xffff; break;
    case 16: h->phoff = UINT64_MAX - 4; break;
    case 17: h->phoff = 1; break;
    case 18: return sizeof(*h) + sizeof(*p) - 1;
    case 19: p->offset = UINT64_MAX - 8; break;
    case 20: p->filesz = UINT64_MAX; p->memsz = UINT64_MAX; break;
    case 21: return sizeof(*image) - 1;
    case 22: p->memsz = p->filesz - 1; break;
    case 23: p->vaddr = 0; break;
    case 24: p->vaddr = 0x800000000000ULL; break;
    case 25: p->vaddr = 0xffff800000000000ULL; break;
    case 26: p->memsz = UINT64_MAX; break;
    case 27: p->flags = 7; break;
    case 28: p->flags = 1; break;
    case 29: p->flags |= 0x80; break;
    case 30: p->align = 3; break;
    case 31: p->vaddr++; h->entry++; break;
    case 32: p->align = 1ULL << 32; break;
    case 33: h->entry = 0; break;
    case 34: h->entry = p->vaddr - 1; break;
    case 35: h->entry = p->vaddr + p->filesz; break;
    case 36: p->flags = ELF_PF_R; break;
    case 37: p->type = 3; break; /* Interpreter */
    case 38: p->type = 2; break; /* Dynamic */
    case 39: p->type = 7; break; /* TLS */
    case 40: h->phnum = 2; image->segments[1] = *p; break; /* Overlap */
    case 41: p->memsz = ((uint64_t)budget + 1) * 4096; break;
    case 42: h->shoff = UINT64_MAX - 8; h->shnum = 1; h->shentsize = 64; break;
    case 43: h->phnum = 2; image->segments[1].type = 0x6474e551;
             image->segments[1].flags = 7; break; /* Executable stack */
    }
    return sizeof(*image);
}

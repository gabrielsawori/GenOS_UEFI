#include "elf_validate.h"
#include "../kernel/utils.h"

#define ELF_PAGE_SIZE 4096ULL
#define ELF_USER_END 0x800000000000ULL
#define ELF_SECTION_SIZE 64

_Static_assert(sizeof(elf_header_t) == 64, "ELF64 header layout");
_Static_assert(sizeof(elf_phdr_t) == 56, "ELF64 program header layout");

static int file_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

static int section_table_valid(const elf_header_t* h, size_t size) {
    /* Sections do not drive loading. Bound their optional table, but do not
     * interpret its entries. Extended counts/indices are unsupported. */
    if (!h->shoff)
        return !h->shnum && !h->shstrndx &&
               (!h->shentsize || h->shentsize == ELF_SECTION_SIZE);
    return h->shoff >= sizeof(*h) && h->shnum && h->shnum < 0xff00 &&
           h->shentsize == ELF_SECTION_SIZE && h->shstrndx < h->shnum &&
           file_range(size, h->shoff, (uint64_t)h->shnum * ELF_SECTION_SIZE);
}

int elf_validate(const uint8_t* binary, size_t size, uint32_t max_pages,
                 elf_image_info_t* info) {
    if (!binary || size < sizeof(elf_header_t) ||
        size > UINTPTR_MAX - (uintptr_t)binary) return 0;
    /* Copy into aligned locals: files and program tables may be unaligned. */
    elf_header_t h;
    memcpy(&h, binary, sizeof(h));
    if (h.magic != ELF_MAGIC || h.elf[0] != 2 || h.elf[1] != 1 || h.elf[2] != 1 ||
        (h.elf[3] != 0 && h.elf[3] != 3) || h.elf[4] ||
        h.type != 2 || h.machine != 62 || h.version != 1 || h.flags ||
        h.ehsize != sizeof(h) || h.phentsize != sizeof(elf_phdr_t) ||
        !h.phnum || h.phnum > ELF_MAX_PROGRAM_HEADERS || h.phoff < sizeof(h) ||
        !file_range(size, h.phoff, (uint64_t)h.phnum * sizeof(elf_phdr_t)) ||
        !section_table_valid(&h, size) || h.entry < ELF_PAGE_SIZE || h.entry >= ELF_USER_END)
        return 0;

    uint32_t pages = 0;
    uint64_t previous_end = 0;
    int entry_found = 0;
    for (unsigned i = 0; i < h.phnum; i++) {
        elf_phdr_t p;
        memcpy(&p, binary + h.phoff + i * sizeof(p), sizeof(p));
        if (!p.type) continue; /* PT_NULL: the other members are undefined. */
        switch (p.type) {
        case ELF_PT_LOAD:
        case 4:          /* PT_NOTE */
        case 6:          /* PT_PHDR */
        case 0x6474e550: /* PT_GNU_EH_FRAME (metadata only) */
        case 0x6474e553: /* PT_GNU_PROPERTY (metadata only) */
            break;
        case 0x6474e551: /* PT_GNU_STACK: user stack is always NX. */
            if (p.flags & ELF_PF_X) return 0;
            break;
        default:
            /* No dynamic linker, interpreter, TLS setup or relocations. */
            return 0;
        }
        if (!file_range(size, p.offset, p.filesz)) return 0;
        if (p.type != ELF_PT_LOAD) continue;
        if (p.filesz > p.memsz || (p.flags & ~7U) ||
            (p.flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)) return 0;
        if (!p.memsz) continue;
        if (!(p.flags & ELF_PF_R) || p.vaddr < ELF_PAGE_SIZE || p.vaddr >= ELF_USER_END ||
            p.memsz > ELF_USER_END - p.vaddr ||
            (p.vaddr & (ELF_PAGE_SIZE - 1)) != (p.offset & (ELF_PAGE_SIZE - 1)) ||
            (p.align > 1 && ((p.align & (p.align - 1)) ||
                            (p.vaddr & (p.align - 1)) != (p.offset & (p.align - 1))))) return 0;
        uint64_t first = p.vaddr & ~(ELF_PAGE_SIZE - 1);
        uint64_t end = (p.vaddr + p.memsz + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1);
        /* PT_LOADs must be sorted. Shared pages would merge permissions or
         * overwrite data, so this loader explicitly refuses them. */
        if (first < previous_end) return 0;
        previous_end = end;
        uint64_t count = (end - first) / ELF_PAGE_SIZE;
        if (count > max_pages - pages) return 0;
        pages += (uint32_t)count;
        /* Policy: entry must be in actual executable file bytes, not BSS or
         * the zero-filled padding that happens to share an executable page. */
        if ((p.flags & ELF_PF_X) && h.entry >= p.vaddr && h.entry - p.vaddr < p.filesz)
            entry_found = 1;
    }
    if (!entry_found) return 0;
    if (info) { info->header = h; info->page_count = pages; }
    return 1;
}

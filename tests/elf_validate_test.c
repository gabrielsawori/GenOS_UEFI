/* Pure validator tests: exact-length guard pages plus malformed mutations. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "elf_fixture.h"

static uint8_t* page;
static size_t page_size;

static int guarded(const void* image, size_t size, uint32_t budget) {
    assert(size <= page_size);
    uint8_t* data = page + page_size - size;
    memcpy(data, image, size);
    elf_image_info_t info, before;
    memset(&info, 0xa5, sizeof(info));
    memcpy(&before, &info, sizeof(info));
    int valid = elf_validate(data, size, budget, &info);
    if (!valid) assert(memcmp(&info, &before, sizeof(info)) == 0);
    assert(memcmp(data, image, size) == 0);
    return valid;
}

static uint32_t random_word(void) {
    static uint32_t seed = 0x421aef67;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

int main(int argc, char** argv) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    void* mapping = mmap(NULL, 3 * page_size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    page = (uint8_t*)mapping + page_size;
    assert(mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0);
    test_elf_t image = test_elf_valid();
    assert(guarded(&image, sizeof(image), 1));
    assert(!guarded(&image, sizeof(image), 0));
    assert(!elf_validate(NULL, sizeof(image), 252, NULL));
    assert(!elf_validate((void*)(UINTPTR_MAX - 7), sizeof(image), 252, NULL));
    /* Validator also accepts unaligned buffers and program-header tables. */
    uint8_t shifted[sizeof(image) + 1];
    memcpy(shifted + 1, &image, sizeof(image));
    assert(elf_validate(shifted + 1, sizeof(image), 252, NULL));
    memcpy(shifted, &image, sizeof(image));
    image.header.phoff++;
    memmove(shifted + image.header.phoff, shifted + sizeof(elf_header_t), sizeof(elf_phdr_t));
    memcpy(shifted, &image.header, sizeof(image.header));
    assert(guarded(shifted, sizeof(image), 252));

    for (unsigned i = 0; i < TEST_ELF_INVALID_CASES; i++) {
        size_t size = test_elf_invalid(&image, i, 252);
        if (guarded(&image, size, 252)) { fprintf(stderr, "case %u accepted\n", i); abort(); }
    }
    image = test_elf_valid();
    image.header.elf[3] = 3; /* Linux ABI uses the same supported static format. */
    image.segments[0].align = 0;
    assert(guarded(&image, sizeof(image), 252));
    image.segments[0].align = 1;
    assert(guarded(&image, sizeof(image), 252));
    image.segments[0].memsz = 252ULL * 4096 - offsetof(test_elf_t, payload);
    assert(guarded(&image, sizeof(image), 252));
    image.segments[0].memsz++;
    assert(!guarded(&image, sizeof(image), 252));

    image = test_elf_valid();
    image.header.phnum = 2;
    image.segments[1] = (elf_phdr_t){.type = ELF_PT_LOAD, .flags = ELF_PF_R | ELF_PF_W,
                                   .vaddr = 0x40002000, .memsz = 8192, .align = 4096};
    assert(guarded(&image, sizeof(image), 3)); /* Includes a pure BSS segment. */
    assert(!guarded(&image, sizeof(image), 2));
    image.segments[1].vaddr = 0x30000000;
    assert(!guarded(&image, sizeof(image), 252)); /* Unsorted loads. */
    image.segments[1] = image.segments[0];
    image.segments[0].memsz = image.segments[0].filesz = 16;
    image.segments[1].vaddr += 16; image.segments[1].offset += 16;
    image.segments[1].memsz = image.segments[1].filesz = 16;
    assert(!guarded(&image, sizeof(image), 252)); /* Disjoint bytes, same page. */
    image = test_elf_valid();
    image.header.phnum = 2;
    image.segments[1].type = 0; image.segments[1].offset = UINT64_MAX;
    assert(guarded(&image, sizeof(image), 252)); /* PT_NULL ignores fields. */

    struct { test_elf_t image; uint8_t sections[64]; } with_sections = {
        .image = test_elf_valid()
    };
    with_sections.image.header.shoff = sizeof(test_elf_t);
    with_sections.image.header.shnum = 1;
    with_sections.image.header.shentsize = 64;
    assert(guarded(&with_sections, sizeof(with_sections), 252));
    assert(!guarded(&with_sections, sizeof(with_sections) - 1, 252));
    with_sections.image.header.shstrndx = 1;
    assert(!guarded(&with_sections, sizeof(with_sections), 252));
    with_sections.image.header.shstrndx = 0;
    with_sections.image.header.shnum = 0;
    assert(!guarded(&with_sections, sizeof(with_sections), 252));

    for (unsigned i = 0; i < 20000; i++) {
        image = test_elf_valid();
        unsigned changes = 1 + random_word() % 8;
        for (unsigned j = 0; j < changes; j++)
            ((uint8_t*)&image)[random_word() % sizeof(image)] ^= (uint8_t)random_word();
        size_t size = random_word() % (sizeof(image) + 1);
        (void)guarded(&image, size, random_word() % 257);
    }
    assert(munmap(mapping, 3 * page_size) == 0);
    for (int i = 1; i < argc; i++) {
        FILE* file = fopen(argv[i], "rb");
        assert(file && fseek(file, 0, SEEK_END) == 0);
        long length = ftell(file);
        assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
        uint8_t* data = malloc((size_t)length);
        assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
        assert(elf_validate(data, (size_t)length, 252, NULL));
        free(data);
        fclose(file);
        printf("PASS executable: %s\n", argv[i]);
    }
    puts("ELF validator PASS: guard pages, 44 rejection cases, boundaries, 20000 mutations.");
}

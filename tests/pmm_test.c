/* Host test with synthetic physical RAM; no privileged instruction is used on
 * the successful initialization path. Private state is visible for overflow. */
#include <assert.h>
#include <stdio.h>
#include "../mm/pmm.c"

volatile struct limine_memmap_request memmap_request;
static unsigned char ram[8 * 1024 * 1024] __attribute__((aligned(4096)));

void serial_write_string(const char* message) { (void)message; }

int main(void) {
    struct limine_hhdm_response hhdm = {.offset = (uint64_t)ram};
    hhdm_request.response = &hhdm;
    struct limine_memmap_entry region = {
        .base = 3, .length = 16 * PAGE_SIZE + 100, .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry* entries[] = {&region};
    struct limine_memmap_response map = {.entry_count = 1, .entries = entries};
    memmap_request.response = &map;
    pmm_init();
    assert(total_pages == 17 && pmm_get_free_ram() == 14 * PAGE_SIZE);
    assert(((uint64_t)refcounts & 3) == 0);
    void* frames[14];
    for (int i = 0; i < 14; i++) {
        frames[i] = pmm_alloc_page();
        assert((uint64_t)frames[i] >= 2 * PAGE_SIZE &&
               (uint64_t)frames[i] < 16 * PAGE_SIZE);
    }
    assert(!pmm_alloc_page() && !pmm_alloc_contiguous_pages(2));
    for (int i = 0; i < 14; i++) pmm_free_page(frames[i]);
    uint64_t baseline = pmm_get_free_ram();
    void* frame = pmm_alloc_page();
    for (int i = 0; i < 300; i++) assert(pmm_ref_page(frame));
    assert(pmm_get_refcount(frame) == 301);
    for (int i = 0; i < 300; i++) pmm_free_page(frame);
    assert(pmm_get_refcount(frame) == 1);
    uint64_t index = (uint64_t)frame / PAGE_SIZE;
    refcounts[index] = UINT32_MAX - 1;
    assert(!pmm_ref_page(frame) && refcounts[index] == UINT32_MAX - 1);
    refcounts[index] = 1;
    pmm_free_page((void*)((uint64_t)frame + 1));
    assert(!pmm_ref_page((void*)((uint64_t)frame + 1)) && refcounts[index] == 1);
    pmm_free_page(frame);
    pmm_free_page(frame);
    pmm_free_page(NULL);
    pmm_free_page((void*)PAGE_SIZE); /* Metadata must remain reserved. */
    assert(!pmm_ref_page(NULL) && !pmm_ref_page((void*)PAGE_SIZE));
    assert(pmm_get_free_ram() == baseline && preempt_depth() == 0);
    /* 1,985 frames require exactly two metadata pages, with no extra page. */
    region.base = 0;
    region.length = 1985 * PAGE_SIZE;
    pmm_init();
    assert(pmm_get_free_ram() == (1985 - 3) * PAGE_SIZE);
    assert(pmm_alloc_contiguous_pages(4) == (void*)(3 * PAGE_SIZE));
    for (int i = 3; i < 7; i++) pmm_free_page((void*)((uint64_t)i * PAGE_SIZE));
    assert(pmm_get_free_ram() == (1985 - 3) * PAGE_SIZE);
    puts("PMM tests passed: boundaries, reservation, 301 refs, overflow, invalid frames.");
}

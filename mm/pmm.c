#include "../cpu/preempt.h"
#include "pmm.h"
#include "../limine.h"
#include "../drivers/serial.h"

// Memanggil Peta Memori dari kernel.c
extern volatile struct limine_memmap_request memmap_request;

// --- PERBAIKAN: Kita hapus kata 'static' agar bisa dipakai oleh vmm.c ---
__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static uint8_t* bitmap = NULL;     
static uint64_t bitmap_size = 0;   
static uint64_t total_pages = 0;   
static uint64_t free_pages = 0;    
static uint64_t highest_address = 0; 
static uint64_t hhdm_offset = 0;

/*
 * Page reference count array.
 * refcounts[page_index] = number of owned references to this physical page.
 *   0 = free (bit clear in bitmap)
 *   1 = exclusively owned (data, stack, table or SHM backing)
 *  >1 = shared (Copy-on-Write)
 *
 * UINT32_MAX marks reserved frames; it is never a releasable reference.
 * Stored at a uint32_t-aligned offset after the bitmap.
 */
static uint32_t* refcounts = NULL;
static uint64_t refcounts_size = 0;

static void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8)); 
}

static void bitmap_clear(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8)); 
}

static int bitmap_test(uint64_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0; 
}

void pmm_init(void) {
    serial_write_string("[INFO] Menginisialisasi Physical Memory Manager (PMM)...\n");

    struct limine_memmap_response *mmap = memmap_request.response;
    
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    }

    if (!mmap) return;

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        int type = mmap->entries[i]->type;
        if (type == LIMINE_MEMMAP_USABLE || 
            type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE || 
            type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            uint64_t top = mmap->entries[i]->base + mmap->entries[i]->length;
            if (top > highest_address) {
                highest_address = top;
            }
        }
    }

    total_pages = (highest_address + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8;
    uint64_t refs_offset = (bitmap_size + sizeof(uint32_t) - 1) & ~3ULL;
    refcounts_size = total_pages * sizeof(uint32_t);
    uint64_t metadata_bytes = refs_offset + refcounts_size;
    uint64_t metadata_pages = (metadata_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t metadata_phys = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t start = (e->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
        uint64_t end = (e->base + e->length) & ~(PAGE_SIZE - 1ULL);
        if (start == 0) start = PAGE_SIZE; /* NULL remains allocation failure. */
        if (end >= start && end - start >= metadata_pages * PAGE_SIZE) {
            metadata_phys = start;
            break;
        }
    }
    if (!metadata_phys) {
        serial_write_string("[FATAL] PMM: insufficient metadata memory\n");
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }
    bitmap = (uint8_t*)(metadata_phys + hhdm_offset);
    refcounts = (uint32_t*)(bitmap + refs_offset);
    for (uint64_t i = 0; i < bitmap_size; i++) bitmap[i] = 0xFF;
    /* UINT32_MAX is reserved, never a releasable allocation. */
    for (uint64_t i = 0; i < total_pages; i++) refcounts[i] = UINT32_MAX;
    free_pages = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t first = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end = (e->base + e->length) / PAGE_SIZE;
        if (first == 0) first = 1;
        for (uint64_t page = first; page < end; page++) {
            if (bitmap_test(page)) free_pages++;
            bitmap_clear(page);
            refcounts[page] = 0;
        }
    }
    for (uint64_t i = 0; i < metadata_pages; i++) {
        uint64_t page = metadata_phys / PAGE_SIZE + i;
        bitmap_set(page);
        refcounts[page] = UINT32_MAX;
        free_pages--;
    }

    serial_write_string("[OK] PMM Aktif. Peta Bitmap telah dikalibrasi.\n");
}

static uint64_t last_alloc_index = 0;

void* pmm_alloc_page(void) {
    PREEMPT_GUARD;
    for (uint64_t i = 0; i < total_pages; i++) {
        uint64_t idx = (last_alloc_index + i) % total_pages;
        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            refcounts[idx] = 1; /* New page starts with refcount=1 */
            free_pages--;
            last_alloc_index = idx + 1;
            return (void*)(idx * PAGE_SIZE);
        }
    }
    return NULL;
}

void* pmm_alloc_contiguous_pages(uint64_t count) {
    PREEMPT_GUARD;
    if (count == 0 || count > free_pages) return NULL;
    if (count == 1) return pmm_alloc_page();

    uint64_t run = 0;
    uint64_t start_idx = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) start_idx = i;
            run++;
            if (run == count) {
                for (uint64_t j = 0; j < count; j++) {
                    bitmap_set(start_idx + j);
                    refcounts[start_idx + j] = 1;
                }
                free_pages -= count;
                return (void*)(start_idx * PAGE_SIZE);
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

static int allocated_frame(void* ptr) {
    uint64_t phys = (uint64_t)ptr;
    uint64_t page = phys / PAGE_SIZE;
    return phys && !(phys & (PAGE_SIZE - 1)) && page < total_pages &&
           bitmap_test(page) && refcounts[page] && refcounts[page] != UINT32_MAX;
}

void pmm_free_page(void* ptr) {
    PREEMPT_GUARD;
    if (!allocated_frame(ptr)) return;
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (--refcounts[page]) return;
    bitmap_clear(page);
    free_pages++;
}

int pmm_ref_page(void* ptr) {
    PREEMPT_GUARD;
    if (!allocated_frame(ptr)) return 0;
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (refcounts[page] == UINT32_MAX - 1) return 0;
    refcounts[page]++;
    return 1;
}

uint32_t pmm_get_refcount(void* ptr) {
    PREEMPT_GUARD;
    return allocated_frame(ptr) ? refcounts[(uint64_t)ptr / PAGE_SIZE] : 0;
}

uint64_t pmm_get_free_ram(void) {
    return free_pages * PAGE_SIZE; 
}

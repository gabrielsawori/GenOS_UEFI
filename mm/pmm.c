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
 * refcounts[page_index] = number of mappings referencing this physical page.
 *   0 = free (bit clear in bitmap)
 *   1 = exclusively owned by one process
 *  >1 = shared (Copy-on-Write)
 *
 * Stored right after bitmap in the same usable memory region.
 */
static uint8_t* refcounts = NULL;
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

    total_pages = highest_address / PAGE_SIZE;
    
    bitmap_size = total_pages / 8;
    if (bitmap_size * 8 < total_pages) bitmap_size++;

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        if (mmap->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            /* Need space for bitmap + refcounts (1 byte per page) */
            refcounts_size = total_pages; /* 1 byte per page */
            uint64_t needed = bitmap_size + refcounts_size;
            if (mmap->entries[i]->length >= needed) {
                bitmap = (uint8_t*)(mmap->entries[i]->base + hhdm_offset);
                refcounts = bitmap + bitmap_size;
                break;
            }
        }
    }

    if (bitmap == NULL) {
        serial_write_string("[FATAL] PMM: Tidak dapat menemukan memori berurutan yang cukup untuk bitmap!\n");
        asm volatile ("cli");
        for (;;) { asm volatile ("hlt"); }
    }

    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF; 
    }
    /* Initialize all refcounts to 0 (free) */
    for (uint64_t i = 0; i < refcounts_size; i++) {
        refcounts[i] = 0;
    }

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        if (mmap->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            uint64_t base = mmap->entries[i]->base;
            uint64_t length = mmap->entries[i]->length;
            
            for (uint64_t j = 0; j < length; j += PAGE_SIZE) {
                bitmap_clear((base + j) / PAGE_SIZE); 
                free_pages++;
            }
        }
    }

    /* Mark bitmap + refcounts region as used */
    uint64_t metadata_start_page = ((uint64_t)bitmap - hhdm_offset) / PAGE_SIZE;
    uint64_t metadata_bytes = bitmap_size + refcounts_size;
    uint64_t metadata_pages = metadata_bytes / PAGE_SIZE + 1;
    for (uint64_t i = 0; i < metadata_pages; i++) {
        bitmap_set(metadata_start_page + i);
        refcounts[metadata_start_page + i] = 1;
        free_pages--;
    }

    serial_write_string("[OK] PMM Aktif. Peta Bitmap telah dikalibrasi.\n");
}

static uint64_t last_alloc_index = 0;

void* pmm_alloc_page(void) {
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
    if (count == 0) return NULL;
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

void pmm_free_page(void* ptr) {
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (page >= total_pages) return;
    if (!bitmap_test(page)) return; /* Already free */

    /* Decrement refcount. Only actually free if refcount reaches 0 */
    if (refcounts[page] > 1) {
        refcounts[page]--;
        return; /* Still shared by other processes */
    }

    /* Refcount is 0 or 1 — actually free the page */
    refcounts[page] = 0;
    bitmap_clear(page); 
    free_pages++;
}

/*
 * pmm_ref_page: Increment reference count for a physical page.
 * Called by CoW fork to share a page between parent and child.
 */
void pmm_ref_page(void* ptr) {
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (page < total_pages && bitmap_test(page)) {
        if (refcounts[page] < 255) { /* Prevent overflow */
            refcounts[page]++;
        }
    }
}

/*
 * pmm_get_refcount: Get reference count for a physical page.
 * Returns 0 if page is free or out of range.
 */
uint8_t pmm_get_refcount(void* ptr) {
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (page < total_pages) {
        return refcounts[page];
    }
    return 0;
}

uint64_t pmm_get_free_ram(void) {
    return free_pages * PAGE_SIZE; 
}
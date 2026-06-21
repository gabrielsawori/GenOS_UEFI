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
            if (mmap->entries[i]->length >= bitmap_size) {
                bitmap = (uint8_t*)(mmap->entries[i]->base + hhdm_offset);
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

    uint64_t bitmap_start_page = ((uint64_t)bitmap - hhdm_offset) / PAGE_SIZE;
    uint64_t bitmap_pages = bitmap_size / PAGE_SIZE + 1;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        bitmap_set(bitmap_start_page + i);
        free_pages--;
    }

    serial_write_string("[OK] PMM Aktif. Peta Bitmap telah dikalibrasi.\n");
}

void* pmm_alloc_page(void) {
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i); 
            free_pages--;
            return (void*)(i * PAGE_SIZE); 
        }
    }
    return NULL; 
}

void pmm_free_page(void* ptr) {
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (page < total_pages) {
        if (bitmap_test(page)) {
            bitmap_clear(page); 
            free_pages++;
        }
    }
}

uint64_t pmm_get_free_ram(void) {
    return free_pages * PAGE_SIZE; 
}
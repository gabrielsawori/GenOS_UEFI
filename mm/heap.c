#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"

// Kita alokasikan Heap di alamat virtual antah-berantah yang sangat tinggi
#define HEAP_START_VIRTUAL 0x100000000000
#define HEAP_INITIAL_SIZE  (16 * 4096) // 16 Petak = 64 KB

// Struktur data (Header) yang menempel di atas setiap irisan memori
struct heap_block {
    size_t size;            // Ukuran irisan ini
    int free;               // 1 = Kosong, 0 = Terpakai
    struct heap_block* next;// Penunjuk ke irisan selanjutnya
};

static struct heap_block* heap_head = NULL;

void heap_init(void) {
    serial_write_string("[INFO] Menginisialisasi Kernel Heap Allocator...\n");

    // 1. Minta 16 petak tanah fisik, lalu petakan (teleportasi) ke alamat Virtual Heap
    for (uint64_t i = 0; i < HEAP_INITIAL_SIZE; i += 4096) {
        void* phys_page = pmm_alloc_page();
        
        // 0b11 (3) = Present + Writable (Ada dan bisa ditulisi)
        vmm_map_page(HEAP_START_VIRTUAL + i, (uint64_t)phys_page, 3);
    }

    // 2. Pada awalnya, Heap adalah satu blok raksasa utuh sebesar 64KB yang Kosong
    heap_head = (struct heap_block*)HEAP_START_VIRTUAL;
    heap_head->size = HEAP_INITIAL_SIZE - sizeof(struct heap_block);
    heap_head->free = 1;
    heap_head->next = NULL;

    serial_write_string("[OK] Kernel Heap siap. 64KB Memori Dinamis tersedia.\n");
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    struct heap_block* curr = heap_head;
    
    // Cari irisan memori dari depan ke belakang (First-Fit)
    while (curr != NULL) {
        // Jika irisan ini Kosong DAN Ukurannya Cukup
        if (curr->free && curr->size >= size) {
            
            // Jika irisan ini JAUH lebih besar dari yang diminta, kita iris (Split) lagi!
            if (curr->size > size + sizeof(struct heap_block)) {
                // Buat Header Blok Baru tepat setelah ukuran yang dipesan
                struct heap_block* new_block = (struct heap_block*)((uint8_t*)curr + sizeof(struct heap_block) + size);
                
                new_block->free = 1;
                new_block->size = curr->size - size - sizeof(struct heap_block);
                new_block->next = curr->next;
                
                curr->size = size;
                curr->next = new_block;
            }
            
            curr->free = 0; // Tandai sedang dipakai (Milik Aplikasi)
            
            // Kembalikan alamat ruang kosongnya (Tepat SETELAH header blok)
            return (void*)((uint8_t*)curr + sizeof(struct heap_block));
        }
        curr = curr->next;
    }
    
    serial_write_string("[ERROR] Heap Out of Memory!\n");
    return NULL; 
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    // Dapatkan header dari pointer ini (mundur beberapa byte)
    struct heap_block* block = (struct heap_block*)((uint8_t*)ptr - sizeof(struct heap_block));
    block->free = 1; // Bebaskan!
    
    // Defragmentasi: Jika blok di sebelahnya ternyata juga kosong, gabungkan (Merge) jadi 1 blok besar!
    if (block->next != NULL && block->next->free) {
        block->size += sizeof(struct heap_block) + block->next->size;
        block->next = block->next->next;
    }
}
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"

// Kita alokasikan Heap di alamat virtual upper-half agar bisa diakses
// dari semua address space (PML4 entries 256-511 di-share ke semua proses).
// 0xFFFFC00000000000 = PML4 entry 496 (canonical, upper half).
#define HEAP_START_VIRTUAL 0xFFFFC00000000000ULL
#define HEAP_INITIAL_SIZE  (16 * 4096) // 16 Petak = 64 KB
#define HEAP_GROW_SIZE     (16 * 4096) // Tumbuh 16 Petak (64 KB) setiap kali habis

// Struktur data (Header) yang menempel di atas setiap irisan memori
struct heap_block {
    size_t size;            // Ukuran irisan ini
    int free;               // 1 = Kosong, 0 = Terpakai
    struct heap_block* next;// Penunjuk ke irisan selanjutnya
};

static struct heap_block* heap_head = NULL;
static uint64_t heap_end = HEAP_START_VIRTUAL + HEAP_INITIAL_SIZE; // Ujung heap saat ini

void heap_init(void) {
    serial_write_string("[INFO] Menginisialisasi Kernel Heap Allocator...\n");

    /*
     * BUG FIX #4: Tambahkan null-check pada pmm_alloc_page().
     * Sebelumnya, jika PMM kehabisan frame (alloc returns NULL = alamat fisik 0),
     * kita tetap memetakan NULL ke alamat virtual heap. Akibatnya:
     *   - vmm_map_page memetakan virtual 0x100000000000 ke fisik 0x0
     *   - Heap header ditulis ke alamat 0x0 → menimpa NULL page / BIOS data
     *   - Kernel panic pada akses heap pertama
     * Sekarang: jika alokasi gagal, hentikan boot dengan pesan error ke serial.
     */
    for (uint64_t i = 0; i < HEAP_INITIAL_SIZE; i += 4096) {
        void* phys_page = pmm_alloc_page();
        if (!phys_page) {
            serial_write_string("[FATAL] heap_init: PMM kehabisan frame! Heap tidak dapat dibuat.\n");
            /* Hentikan sistem dengan aman */
            asm volatile ("cli");
            for (;;) { asm volatile ("hlt"); }
        }
        /* 0x03 = Present + Writable (tanpa User bit = hanya kernel yang bisa akses) */
        vmm_map_page(HEAP_START_VIRTUAL + i, (uint64_t)phys_page, 3);
    }

    /* Pada awalnya, Heap adalah satu blok raksasa utuh sebesar 64KB yang Kosong */
    heap_head = (struct heap_block*)HEAP_START_VIRTUAL;
    heap_head->size = HEAP_INITIAL_SIZE - sizeof(struct heap_block);
    heap_head->free = 1;
    heap_head->next = NULL;

    serial_write_string("[OK] Kernel Heap siap. 64KB Memori Dinamis tersedia.\n");
}

/*
 * heap_grow: Memperluas heap sebesar HEAP_GROW_SIZE byte.
 *
 * Ketika kmalloc kehabisan blok kosong, fungsi ini dipanggil untuk
 * meminta page baru dari PMM, memetakannya ke VMM, lalu menambahkan
 * blok kosong baru di ujung linked-list heap. Dengan begitu, heap
 * bisa tumbuh melampaui 64KB awal tanpa batas (selama RAM tersedia).
 */
static int heap_grow(void) {
    serial_write_string("[INFO] Heap berkembang...\n");

    /* Cari blok terakhir di linked-list */
    struct heap_block* last = heap_head;
    while (last->next != NULL) {
        last = last->next;
    }

    /* Alokasikan page baru dari PMM dan petakan ke VMM */
    for (uint64_t i = 0; i < HEAP_GROW_SIZE; i += 4096) {
        void* phys_page = pmm_alloc_page();
        if (!phys_page) {
            serial_write_string("[FATAL] heap_grow: PMM kehabisan frame!\n");
            return 0;
        }
        vmm_map_page(heap_end + i, (uint64_t)phys_page, 3);
    }

    /* Buat blok kosong baru di ujung heap */
    struct heap_block* new_block = (struct heap_block*)heap_end;
    new_block->size = HEAP_GROW_SIZE - sizeof(struct heap_block);
    new_block->free = 1;
    new_block->next = NULL;

    /* Sambungkan ke blok terakhir */
    last->next = new_block;

    /* Jika blok terakhir juga kosong, gabungkan (forward merge) */
    if (last->free) {
        last->size += sizeof(struct heap_block) + new_block->size;
        last->next = NULL;
    }

    heap_end += HEAP_GROW_SIZE;
    serial_write_string("[OK] Heap berkembang 64KB.\n");
    return 1;
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
    
    // Tidak ada blok yang cocok. Coba perluas heap lalu cari lagi.
    if (heap_grow()) {
        return kmalloc(size);
    }

    serial_write_string("[ERROR] Heap Out of Memory!\n");
    return NULL; 
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    // Dapatkan header dari pointer ini (mundur beberapa byte)
    struct heap_block* block = (struct heap_block*)((uint8_t*)ptr - sizeof(struct heap_block));
    block->free = 1; // Bebaskan!
    
    // Defragmentasi #1: Gabung dengan blok SEBELUMNYA (Backward Coalescing)
    // Cari blok yang menunjuk ke blok ini (prev) dengan scan dari head.
    struct heap_block* prev = NULL;
    struct heap_block* walk = heap_head;
    while (walk != NULL && walk != block) {
        prev = walk;
        walk = walk->next;
    }
    
    if (prev != NULL && prev->free) {
        // Blok sebelumnya kosong → gabungkan jadi satu blok besar
        prev->size += sizeof(struct heap_block) + block->size;
        prev->next = block->next;
        block = prev; // Sekarang "block" menunjuk ke blok gabungan
    }
    
    // Defragmentasi #2: Gabung dengan blok SETELAHNYA (Forward Coalescing)
    if (block->next != NULL && block->next->free) {
        block->size += sizeof(struct heap_block) + block->next->size;
        block->next = block->next->next;
    }
}

/*
 * krealloc: Mengubah ukuran alokasi memori yang sudah ada.
 *
 * @param ptr: pointer dari kmalloc sebelumnya (atau NULL).
 * @param new_size: ukuran baru yang diminta.
 *
 * Perilaku:
 *   - ptr == NULL  → sama dengan kmalloc(new_size)
 *   - new_size == 0 → sama dengan kfree(ptr), return NULL
 *   - blok sudah cukup besar → return ptr (tanpa copy)
 *   - blok terlalu kecil → alokasi baru, copy data lama, bebas lama
 */
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    /* Dapatkan header blok lama untuk cek ukurannya */
    struct heap_block* old_block = (struct heap_block*)((uint8_t*)ptr - sizeof(struct heap_block));
    size_t old_size = old_block->size;

    /* Jika blok lama sudah cukup besar, langsung return */
    if (old_size >= new_size) return ptr;

    /* Alokasi blok baru, copy data lama, bebaskan blok lama */
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    /* Copy data lama ke blok baru (manual memcpy — libc tidak tersedia di kernel) */
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < old_size; i++) {
        dst[i] = src[i];
    }

    kfree(ptr);
    return new_ptr;
}
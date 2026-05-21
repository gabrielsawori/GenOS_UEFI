#include "gdt.h"
#include "../drivers/serial.h"

// Kita membuat 5 entri GDT dasar untuk GenOS
uint64_t gdt_entries[5];

// Pointer untuk memberi tahu CPU di mana lokasi GDT kita
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct gdt_ptr gdtr;

// Fungsi Assembly yang akan kita buat nanti untuk memuat GDT ke CPU
extern void gdt_load(uint64_t ptr);

void gdt_init(void) {
    serial_write_string("[INFO] Mengonfigurasi Global Descriptor Table (GDT)...\n");

    // 0: Null Descriptor (Wajib kosong menurut aturan CPU intel/AMD)
    gdt_entries[0] = 0;
    
    // 1: Kernel Code Segment (Ring 0, Executable, 64-bit)
    gdt_entries[1] = 0x00209A0000000000;
    
    // 2: Kernel Data Segment (Ring 0, Read/Write)
    gdt_entries[2] = 0x0000920000000000;
    
    // 3: User Data Segment (Ring 3, Read/Write) -> Untuk aplikasi masa depan
    gdt_entries[3] = 0x0000F20000000000;
    
    // 4: User Code Segment (Ring 3, Executable, 64-bit)
    gdt_entries[4] = 0x0020FA0000000000;

    // Atur panjang tabel dan alamat awalnya
    gdtr.limit = sizeof(gdt_entries) - 1;
    gdtr.base = (uint64_t)&gdt_entries;

    // Berikan tabel ini ke CPU melalui fungsi Assembly!
    gdt_load((uint64_t)&gdtr);
    
    serial_write_string("[OK] GDT Berhasil Dimuat!\n");
}
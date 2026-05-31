#include "gdt.h"
#include "../drivers/serial.h"
#include <stddef.h>

// Struktur TSS (Task State Segment) x86_64
struct tss {
    uint32_t reserved0;
    uint64_t rsp0; // INI YANG PALING PENTING! (Pointer Stack Darurat Kernel)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

static struct tss tss_entry;

// Array GDT kita sekarang butuh 7 slot
static uint64_t gdt[7];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdtr gdtr;

extern void gdt_load(uint64_t ptr);

void gdt_init(void) {
    serial_write_string("[INFO] Mengonfigurasi GDT v2 (Menambahkan Ring 3 & TSS)...\n");

    // 1. Segmen Dasar
    gdt[0] = 0x0000000000000000; // Null Descriptor
    gdt[1] = 0x00AF9A000000FFFF; // Kernel Code (Ring 0)
    gdt[2] = 0x00CF92000000FFFF; // Kernel Data (Ring 0)
    
    // 2. Segmen Kota Biasa (Ring 3) -> DPL = 3
    gdt[3] = 0x00CFF2000000FFFF; // User Data (Ring 3)
    gdt[4] = 0x00AFFA000000FFFF; // User Code (Ring 3)

    // 3. Bersihkan struktur TSS (Perbaikan Warning size_t)
    for(size_t i = 0; i < sizeof(struct tss); i++) {
        ((uint8_t*)&tss_entry)[i] = 0;
    }
    tss_entry.iopb_offset = sizeof(struct tss);

    // 4. Hitung alamat untuk memasukkan TSS ke GDT
    uint64_t tss_base = (uint64_t)&tss_entry;
    uint32_t tss_limit = sizeof(struct tss) - 1;

    uint32_t base_low = tss_base & 0xFFFFFFFF;
    uint32_t base_high = tss_base >> 32;

    // TSS di 64-bit memakan 2 slot (Perbaikan Warning Shift Overflow dengan (uint64_t))
    gdt[5] = (tss_limit & 0xFFFF) | ((base_low & 0xFFFFFF) << 16) | 
             (0x89ULL << 40) | (((uint64_t)((tss_limit >> 16) & 0xF)) << 48) | 
             (((uint64_t)(base_low >> 24)) << 56);
    gdt[6] = base_high;

    // 5. Muat GDT ke CPU
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt[0];
    gdt_load((uint64_t)&gdtr);

    // 6. BERI TAHU CPU letak TSS
    asm volatile ("ltr %w0" : : "r" (0x28));

    serial_write_string("[OK] GDT v2 & TSS Berhasil Dimuat!\n");
}

void set_kernel_stack(uint64_t stack) {
    tss_entry.rsp0 = stack;
}
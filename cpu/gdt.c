#include "gdt.h"
#include "../drivers/serial.h"
#include <stddef.h>

/*
 * BSP's TSS and GDT. APs create their own copies via percpu.c.
 * TSS struct is now defined in gdt.h (shared with percpu.c).
 */
static struct tss tss_entry;

/* GDT: 7 slots = Null + KCode + KData + UData + UCode + TSS(2 slots) */
static uint64_t gdt[7];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdtr gdtr;

void gdt_init(void) {
    serial_write_string("[INFO] Mengonfigurasi GDT v2 (Menambahkan Ring 3 & TSS)...\n");

    /* Segment descriptors (shared layout for all CPUs) */
    gdt[0] = 0x0000000000000000; /* Null Descriptor */
    gdt[1] = 0x00AF9A000000FFFF; /* Kernel Code (Ring 0) */
    gdt[2] = 0x00CF92000000FFFF; /* Kernel Data (Ring 0) */
    gdt[3] = 0x00CFF2000000FFFF; /* User Data (Ring 3)   */
    gdt[4] = 0x00AFFA000000FFFF; /* User Code (Ring 3)   */

    /* Clear TSS */
    for (size_t i = 0; i < sizeof(struct tss); i++) {
        ((uint8_t*)&tss_entry)[i] = 0;
    }
    tss_entry.iopb_offset = sizeof(struct tss);

    /* Encode TSS descriptor into GDT slots 5-6 */
    uint64_t tss_base = (uint64_t)&tss_entry;
    uint32_t tss_limit = sizeof(struct tss) - 1;
    uint32_t base_low = tss_base & 0xFFFFFFFF;
    uint32_t base_high = tss_base >> 32;

    /*
     * BUG FIX: Cast base_low to uint64_t before shifting.
     *
     * (base_low & 0xFFFFFF) can be up to 24 bits. Shifting left by 16
     * produces up to 40 bits, which overflows uint32_t (32 bits).
     * When tss_entry is at an address where bits 16-23 of the low
     * 32 bits are non-zero (e.g., 0x80014ca0 → 0x014ca0), the top
     * bits are silently truncated, causing TSS base to be wrong.
     *
     * This made every Ring 3→0 stack switch read RSP0 from garbage
     * memory → #SS → #DF → triple fault on the first interrupt
     * while running user-mode code.
     */
    gdt[5] = (tss_limit & 0xFFFF) | ((uint64_t)(base_low & 0xFFFFFF) << 16) |
             (0x89ULL << 40) | (((uint64_t)((tss_limit >> 16) & 0xF)) << 48) |
             (((uint64_t)(base_low >> 24)) << 56);
    gdt[6] = base_high;

    /* Load GDT */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt[0];
    gdt_load((uint64_t)&gdtr);

    /* Load TSS selector (offset 0x28 = slot 5) */
    asm volatile("ltr %w0" : : "r" (0x28));

    serial_write_string("[OK] GDT v2 & TSS Berhasil Dimuat!\n");
}

void set_kernel_stack(uint64_t stack) {
    tss_entry.rsp0 = stack;
}

void gdt_set_ist1(uint64_t ist_stack) {
    tss_entry.ist1 = ist_stack;
}

void gdt_load_on_ap(void) {
    gdt_load((uint64_t)&gdtr);
}

uint64_t* gdt_get_entries(void) {
    return gdt;
}

uint16_t gdt_get_entry_count(void) {
    return 7;
}
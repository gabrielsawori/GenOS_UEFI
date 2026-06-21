#include "idt.h"
#include "../drivers/serial.h"

struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

__attribute__((aligned(0x10)))
static struct idt_entry idt[256];
static struct idtr idtr;

extern void idt_load(uint64_t ptr);
extern uint64_t isr_stub_table[];
extern uint64_t irq_stub_table[]; // Tabel IRQ baru

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    uint64_t descriptor = (uint64_t)isr;
    idt[vector].isr_low    = (uint16_t)(descriptor & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; 
    idt[vector].ist        = 0;
    idt[vector].attributes = flags;
    idt[vector].isr_mid    = (uint16_t)((descriptor >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)(descriptor >> 32);
    idt[vector].reserved   = 0;
}

void idt_init(void) {
    serial_write_string("[INFO] Mengonfigurasi Interrupt Descriptor Table (IDT)...\n");

    idtr.base = (uint64_t)&idt[0];
    idtr.limit = (uint16_t)sizeof(struct idt_entry) * 256 - 1;

    // 1. Daftarkan 32 Error CPU (Kamar 0 - 31)
    for (uint8_t i = 0; i < 32; i++) {
        idt_set_descriptor(i, (void*)isr_stub_table[i], 0x8E);
    }

    // 2. Daftarkan 16 Hardware IRQ (Kamar 32 - 47)
    for (uint8_t i = 0; i < 16; i++) {
        idt_set_descriptor(32 + i, (void*)irq_stub_table[i], 0x8E);
    }

    // 3. LAPIC Timer (Vector 48) — for SMP per-CPU scheduling
    extern void lapic_timer_stub(void);
    idt_set_descriptor(48, (void*)lapic_timer_stub, 0x8E);

    idt_load((uint64_t)&idtr);
    serial_write_string("[OK] IDT Berhasil Dimuat dan ISR/IRQ Didaftarkan!\n");
}

void idt_load_on_ap(void) {
    idt_load((uint64_t)&idtr);
}
#pragma once
#include <stdint.h>

// Fungsi untuk memindahkan offset IRQ hardware ke kamar 32+ di IDT
void pic_remap(void);

/*
 * pic_unmask_irq() - Izinkan suatu IRQ lewat ke CPU (clear bit di mask register).
 *
 * IRQ 0-7  → Master PIC (port 0x21), bit = irq
 * IRQ 8-15 → Slave PIC  (port 0xA1), bit = irq - 8
 *
 * Dipakai untuk mengaktifkan IRQ12 (mouse) dan IRQ2 (cascade master→slave).
 */
void pic_unmask_irq(uint8_t irq);
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
/* QEMU PC ELCR: PCI INTx is level-triggered. Reserved ISA/PS2 lines rejected. */
int pic_enable_pci_irq(uint8_t irq);
/* Begin rejects spurious IRQ7/15. End acknowledges a real IRQ after devices. */
int pic_begin_irq(uint8_t irq);
void pic_end_irq(uint8_t irq);
struct pic_irq_stats { uint64_t spurious7, spurious15; };
void pic_get_irq_stats(struct pic_irq_stats* out);

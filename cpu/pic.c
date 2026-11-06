#include "pic.h"
#include "irq_lock.h"
#include "../drivers/io.h"
#include "../drivers/serial.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01
static spinlock_t pic_lock = SPINLOCK_INIT;
static struct pic_irq_stats stats;

void pic_remap(void) {
    serial_write_string("[INFO] Melakukan Remapping PIC...\n");

    /*
     * BUG FIX: Explicitly disable interrupts before reprogramming PIC.
     * On bare metal, stale IRQs from BIOS (e.g. timer, USB) can fire
     * during the ICW sequence, corrupting the partially-configured PIC.
     */
    asm volatile ("cli");

    /* ICW1: Begin initialization sequence */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: Set vector offsets */
    outb(PIC1_DATA, 0x20);  /* Master: IRQ 0-7  → INT 32-39 */
    io_wait();
    outb(PIC2_DATA, 0x28);  /* Slave:  IRQ 8-15 → INT 40-47 */
    io_wait();

    /* ICW3: Cascade wiring */
    outb(PIC1_DATA, 4);     /* Master: slave on IRQ2 (bit 2) */
    io_wait();
    outb(PIC2_DATA, 2);     /* Slave: cascade identity = 2 */
    io_wait();

    /* ICW4: Set 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* OCW1: Interrupt mask
     * 0xFC = 11111100b → IRQ0 (timer) and IRQ1 (keyboard) enabled */
    outb(PIC1_DATA, 0xFC);
    io_wait();
    outb(PIC2_DATA, 0xFF);  /* Slave: all masked */
    io_wait();

    serial_write_string("[OK] PIC diremap! Timer (IRQ0) dan Keyboard (IRQ1) diizinkan lewat.\n");
}

void pic_unmask_irq(uint8_t irq) {
    if (irq >= 16) return;
    IRQ_LOCK_GUARD(&pic_lock);
    uint16_t port;
    uint8_t  bit;
    if (irq < 8) {
        port = PIC1_DATA;   /* Master PIC mask register (0x21) */
        bit  = irq;
    } else {
        port = PIC2_DATA;   /* Slave PIC mask register (0xA1) */
        bit  = irq - 8;
    }
    /* Clear bit untuk mengizinkan IRQ ini lewat (bit=1 = masked, bit=0 = enabled) */
    outb(port, inb(port) & ~(1 << bit));
}

int pic_enable_pci_irq(uint8_t irq) {
    /* ELCR writable bits are DEF8; leave PS/2 IRQ12 edge-triggered. */
    if (irq >= 16 || !(0xcef8U & (1U << irq))) return 0;
    IRQ_LOCK_GUARD(&pic_lock);
    uint16_t elcr = 0x4d0 + irq / 8;
    uint8_t mask = 1U << (irq & 7);
    outb(elcr, inb(elcr) | mask);
    if (!(inb(elcr) & mask)) return 0;
    if (irq >= 8) outb(PIC1_DATA, inb(PIC1_DATA) & ~(1U << 2));
    uint16_t port = irq >= 8 ? PIC2_DATA : PIC1_DATA;
    outb(port, inb(port) & ~mask);
    return 1;
}

int pic_begin_irq(uint8_t irq) {
    if (irq >= 16) return 0;
    if (irq != 7 && irq != 15) return 1;
    IRQ_LOCK_GUARD(&pic_lock);
    uint16_t command = irq == 7 ? PIC1_COMMAND : PIC2_COMMAND;
    outb(command, 0x0b); /* OCW3: read in-service register. */
    if (inb(command) & 0x80) return 1;
    if (irq == 7) stats.spurious7++;
    else {
        stats.spurious15++;
        outb(PIC1_COMMAND, 0x20); /* Cascade was real; slave IRQ15 was not. */
    }
    return 0;
}
void pic_end_irq(uint8_t irq) {
    IRQ_LOCK_GUARD(&pic_lock);
    if (irq >= 8) outb(PIC2_COMMAND, 0x20);
    outb(PIC1_COMMAND, 0x20);
}
void pic_get_irq_stats(struct pic_irq_stats* out) {
    IRQ_LOCK_GUARD(&pic_lock);
    if (out) *out = stats;
}

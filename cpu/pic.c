#include "pic.h"
#include "../drivers/io.h"
#include "../drivers/serial.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

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
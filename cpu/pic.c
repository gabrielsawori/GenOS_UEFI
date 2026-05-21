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

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, 0x20); 
    outb(PIC2_DATA, 0x28); 

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    // KUNCI: Biner 1111 1100 = Heksadesimal 0xFC
    // Bit 0 = 0 (Timer Dibuka)
    // Bit 1 = 0 (Keyboard Dibuka)
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);

    serial_write_string("[OK] PIC diremap! Timer (IRQ0) dan Keyboard (IRQ1) diizinkan lewat.\n");
}
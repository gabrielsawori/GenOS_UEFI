#include "isr.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/io.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h" 
#include "../kernel/task.h" // Masukkan Header Scheduler Kita!

const char *exception_messages[32] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception", "VMM Communication Exception", "Security Exception", "Reserved"
};

void isr_handler(struct registers *regs) {
    if (regs->int_no < 32) {
        serial_write_string("\n[!!!] KERNEL PANIC: ");
        serial_write_string(exception_messages[regs->int_no]);
        serial_write_string(" [!!!]\n");

        fb_print("=============================================", 50, 300, 0xFF0000, 0x002244, 2);
        fb_print("                KERNEL PANIC!                ", 50, 320, 0xFF0000, 0x002244, 2);
        fb_print("=============================================", 50, 340, 0xFF0000, 0x002244, 2);
        fb_print(exception_messages[regs->int_no], 50, 410, 0xFFFF00, 0x002244, 2);

        asm volatile ("cli");
        for (;;) { asm volatile ("hlt"); }
    } 
    else if (regs->int_no >= 32 && regs->int_no <= 47) {
        
        // JIKA DETAK TIMER (IRQ 0 -> Offset 32)
        if (regs->int_no == 32) {
            timer_tick(); 
            // --- KEAJAIBAN TERJADI DI SINI ---
            schedule(regs); // Ganti program 1000x per detik!
        }
        // JIKA KETIKAN KEYBOARD (IRQ 1 -> Offset 33)
        else if (regs->int_no == 33) {
            uint8_t scancode = inb(0x60); 
            keyboard_handler(scancode);   
        }

        if (regs->int_no >= 40) { outb(0xA0, 0x20); }
        outb(0x20, 0x20);
    }
}
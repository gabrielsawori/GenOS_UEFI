#include "isr.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/io.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"

/*
 * Tabel nama exception CPU x86-64 (INT 0 - 31).
 * Digunakan oleh isr_handler untuk menampilkan nama error yang mudah dibaca.
 */
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

/*
 * print_hex_serial() - Cetak angka dalam format hexadecimal ke serial port.
 *
 * @param label: teks label yang ditampilkan sebelum angka (misal "CR2")
 * @param value: nilai 64-bit yang akan dicetak dalam hex
 *
 * Output format: "  label = 0xHEXVALUE\n"
 */
static void print_hex_serial(const char* label, uint64_t value) {
    char buf[20];
    serial_write_string("  ");
    serial_write_string(label);
    serial_write_string(" = 0x");
    itoa(value, buf, 16);
    serial_write_string(buf);
    serial_write_string("\n");
}

/*
 * print_hex_fb() - Cetak "label = 0xVALUE" ke framebuffer pada posisi tertentu.
 *
 * @param label: teks label
 * @param value: nilai 64-bit
 * @param x, y: koordinat piksel pada framebuffer
 * @param color: warna teks (format 0xRRGGBB)
 */
static void print_hex_fb(const char* label, uint64_t value, size_t x, size_t y, uint32_t color) {
    char buf[20];
    fb_print(label, x, y, color, 0x002244, 2);
    /* Hitung posisi setelah label (setiap karakter = 16px pada scale 2) */
    size_t label_len = 0;
    while (label[label_len]) label_len++;
    size_t val_x = x + (label_len * 16);
    fb_print("0x", val_x, y, color, 0x002244, 2);
    itoa(value, buf, 16);
    fb_print(buf, val_x + 32, y, 0xFFFFFF, 0x002244, 2);
}

/*
 * handle_page_fault() - Handler khusus untuk Page Fault (INT 14).
 *
 * Page Fault adalah exception paling umum dalam pengembangan OS.
 * Handler ini menampilkan informasi diagnostik lengkap:
 *
 *   1. CR2 (Faulting Address) — alamat virtual yang menyebabkan fault
 *   2. Error Code bits:
 *      - Bit 0 (P):    0 = halaman tidak ada, 1 = pelanggaran proteksi
 *      - Bit 1 (W/R):  0 = akses baca, 1 = akses tulis
 *      - Bit 2 (U/S):  0 = mode kernel (Ring 0), 1 = mode user (Ring 3)
 *      - Bit 3 (RSVD): 1 = reserved bit di page table entry terset
 *      - Bit 4 (I/D):  1 = fault saat instruction fetch (eksekusi kode)
 *   3. RIP — alamat instruksi yang menyebabkan fault
 *   4. CS  — code segment selector (0x08 = kernel, 0x23 = user)
 *
 * Semua informasi dicetak ke serial port DAN ke framebuffer agar bisa
 * dilihat baik di QEMU serial console maupun di layar langsung.
 */
static void handle_page_fault(struct registers *regs) {
    /* Baca CR2 — register khusus CPU yang menyimpan alamat fault */
    uint64_t cr2;
    asm volatile ("mov %%cr2, %0" : "=r"(cr2));

    uint64_t err = regs->err_code;

    /* ==================== OUTPUT KE SERIAL ==================== */
    serial_write_string("\n========== PAGE FAULT DIAGNOSTIC ==========\n");
    print_hex_serial("CR2 (Fault Address)", cr2);
    print_hex_serial("RIP (Instruction)  ", regs->rip);
    print_hex_serial("CS  (Code Segment) ", regs->cs);
    print_hex_serial("Error Code         ", err);
    serial_write_string("  Penyebab: ");
    serial_write_string((err & 0x01) ? "Protection Violation" : "Page Not Present");
    serial_write_string("\n  Operasi:  ");
    serial_write_string((err & 0x02) ? "WRITE (Tulis)" : "READ (Baca)");
    serial_write_string("\n  Konteks:  ");
    serial_write_string((err & 0x04) ? "User Mode (Ring 3)" : "Kernel Mode (Ring 0)");
    if (err & 0x08) serial_write_string("\n  [!] Reserved bit set dalam page table!");
    if (err & 0x10) serial_write_string("\n  [!] Instruction Fetch (NX violation)");
    serial_write_string("\n=============================================\n");

    /* ==================== OUTPUT KE LAYAR ==================== */
    size_t y = 300;
    fb_print("============= PAGE FAULT =============", 50, y, 0xFF4444, 0x002244, 2);
    y += 28;
    print_hex_fb("Alamat Fault (CR2): ", cr2, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("Instruksi (RIP):    ", regs->rip, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("Code Segment (CS):  ", regs->cs, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("Error Code:         ", err, 50, y, 0xFF8800);
    y += 28;

    /* Tampilkan penjelasan penyebab yang mudah dibaca manusia */
    fb_print((err & 0x01) ? "Penyebab: Protection Violation"
                          : "Penyebab: Page Not Present", 50, y, 0xFFFF00, 0x002244, 2);
    y += 22;
    fb_print((err & 0x02) ? "Operasi:  WRITE (Tulis)"
                          : "Operasi:  READ (Baca)", 50, y, 0xFFFF00, 0x002244, 2);
    y += 22;
    fb_print((err & 0x04) ? "Konteks:  User Mode (Ring 3)"
                          : "Konteks:  Kernel Mode (Ring 0)", 50, y, 0xFFFF00, 0x002244, 2);
}

/*
 * handle_gpf() - Handler khusus untuk General Protection Fault (INT 13).
 *
 * GPF sering disebabkan oleh:
 *   - Selector segment yang tidak valid (SS, CS menunjuk ke slot GDT yang salah)
 *   - Privilege violation (kode Ring-3 mengakses resource Ring-0)
 *   - Stack alignment error pada mode 64-bit
 *
 * Menampilkan error code (biasanya selector yang bermasalah), RIP, dan CS.
 */
static void handle_gpf(struct registers *regs) {
    serial_write_string("\n========== GENERAL PROTECTION FAULT ==========\n");
    print_hex_serial("Error Code (Selector?)", regs->err_code);
    print_hex_serial("RIP (Instruction)     ", regs->rip);
    print_hex_serial("CS  (Code Segment)    ", regs->cs);
    print_hex_serial("RSP (Stack Pointer)   ", regs->rsp);
    print_hex_serial("SS  (Stack Segment)   ", regs->ss);
    serial_write_string("================================================\n");

    size_t y = 300;
    fb_print("======== GENERAL PROTECTION FAULT ========", 50, y, 0xFF4444, 0x002244, 2);
    y += 28;
    print_hex_fb("Error Code:  ", regs->err_code, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("RIP:         ", regs->rip, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("CS:          ", regs->cs, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("RSP:         ", regs->rsp, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("SS:          ", regs->ss, 50, y, 0xFF8800);
}

/*
 * isr_handler() - Dispatcher utama interrupt & exception.
 *
 * Fungsi ini dipanggil oleh isr_common (assembly) setiap kali CPU menerima
 * interrupt. Berdasarkan nomor interrupt, fungsi ini mengarahkan ke:
 *
 *   INT 0-31:  Exception CPU → Kernel Panic dengan diagnostik
 *     - INT 13: General Protection Fault (handler khusus)
 *     - INT 14: Page Fault (handler khusus dengan CR2)
 *     - Lainnya: handler generik
 *   INT 32-47: Hardware IRQ
 *     - IRQ 0 (INT 32): Timer → scheduler context switch
 *     - IRQ 1 (INT 33): Keyboard → input handler
 *
 * @param regs: pointer ke struct registers yang berisi snapshot CPU saat
 *              interrupt terjadi (disimpan oleh isr_common di stack).
 */
void isr_handler(struct registers *regs) {
    if (regs->int_no < 32) {
        /* ===== CPU EXCEPTION: KERNEL PANIC ===== */
        serial_write_string("\n[!!!] KERNEL PANIC: ");
        serial_write_string(exception_messages[regs->int_no]);
        serial_write_string(" [!!!]\n");

        /* Handler khusus untuk exception yang sering terjadi */
        if (regs->int_no == 14) {
            /* INT 14: Page Fault — tampilkan CR2 dan decode error code */
            handle_page_fault(regs);
        } else if (regs->int_no == 13) {
            /* INT 13: General Protection Fault — tampilkan selector info */
            handle_gpf(regs);
        } else {
            /* Exception lainnya: tampilkan info dasar */
            fb_print("=============================================", 50, 300, 0xFF0000, 0x002244, 2);
            fb_print("                KERNEL PANIC!                ", 50, 320, 0xFF0000, 0x002244, 2);
            fb_print("=============================================", 50, 340, 0xFF0000, 0x002244, 2);
            fb_print(exception_messages[regs->int_no], 50, 410, 0xFFFF00, 0x002244, 2);

            /* Tampilkan RIP dan error code untuk semua exception */
            print_hex_fb("RIP: ", regs->rip, 50, 440, 0xFF8800);
            print_hex_fb("ERR: ", regs->err_code, 50, 464, 0xFF8800);

            print_hex_serial("RIP", regs->rip);
            print_hex_serial("Error Code", regs->err_code);
        }

        asm volatile ("cli");
        for (;;) { asm volatile ("hlt"); }
    }
    else if (regs->int_no >= 32 && regs->int_no <= 47) {
        /* ===== HARDWARE IRQ ===== */

        /* IRQ 0 (INT 32): Timer PIT — detak jantung scheduler */
        if (regs->int_no == 32) {
            timer_tick();
            schedule(regs); /* Context switch 1000x per detik */
        }
        /* IRQ 1 (INT 33): Keyboard PS/2 — input pengguna */
        else if (regs->int_no == 33) {
            uint8_t scancode = inb(0x60);
            keyboard_handler(scancode);
        }

        /* Kirim EOI (End of Interrupt) ke PIC */
        if (regs->int_no >= 40) { outb(0xA0, 0x20); } /* Slave PIC */
        outb(0x20, 0x20); /* Master PIC */
    }
}
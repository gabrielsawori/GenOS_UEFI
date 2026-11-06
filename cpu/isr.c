#include "isr.h"
#include "tlb.h"
#include "pic.h"
#include "../drivers/virtio_irq.h"
#include "../drivers/ps2.h"
#include "lapic.h"
#include "smp.h"
#include "uaccess.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/io.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "preempt.h"
#include "percpu.h"

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
 * log_bad_cs() - Called from isr_common assembly when a garbage CS value
 * is detected on the iretq stack frame. Logs the bad value to serial
 * so we can diagnose which task's state was corrupted.
 */
void log_bad_cs(uint64_t bad_cs) {
    serial_write_string("\n[!!!] BAD CS DETECTED BEFORE IRETQ: 0x");
    char buf[20];
    itoa(bad_cs, buf, 16);
    serial_write_string(buf);
    serial_write_string(" PID=");
    itoa(get_current_pid(), buf, 10);
    serial_write_string(buf);
    serial_write_string(" — FORCED TO 0x08 (kernel)\n");
}

#include "../mm/cow.h"

/* Only synchronous application exceptions are attributable to the current
 * process. NMI, double fault, machine check and reserved vectors remain fatal,
 * even when they interrupted CPL3. There is no userspace signal/debugger ABI. */
static int user_exception(uint64_t vector) {
    switch (vector) {
    case 0: case 1: case 3: case 4: case 5: case 6: case 7:
    case 10: case 11: case 12: case 13: case 14: case 16: case 17:
    case 19: case 21:
        return 1;
    default:
        return 0;
    }
}

static void __attribute__((noreturn)) abort_user(struct registers* regs) {
    {
        PREEMPT_GUARD;
        serial_write_string("[USER TRAP] stopping address-space group\n");
        print_hex_serial("CPU", smp_get_current_cpu_id());
        print_hex_serial("PID", get_current_pid());
        print_hex_serial("Vector", regs->vector);
        print_hex_serial("RIP", regs->rip);
        print_hex_serial("Error", regs->err_code);
        if (regs->vector == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            print_hex_serial("CR2", cr2);
        }
    }
    task_abort_current_group();
}

static void handle_page_fault(struct registers *regs) {
    /* Baca CR2 — register khusus CPU yang menyimpan alamat fault */
    uint64_t cr2;
    asm volatile ("mov %%cr2, %0" : "=r"(cr2));

    uint64_t err = regs->err_code;

    /*
     * === COPY-ON-WRITE RESOLUTION ===
     * Try CoW first. If the fault is a write to a CoW page,
     * resolve it transparently and return to the faulting instruction.
     */
    if (!(err & 8) && cow_resolve(cr2, err)) {
        return; /* CoW handled — resume user code */
    }
    if (!(err & 8) && uaccess_fixup(regs, cr2)) return;

    int user_mode = (regs->cs & 3) == 3; /* Ring 3 = user mode */
    if (user_mode && !(err & 8) && percpu_current()->current &&
        percpu_current()->current->pml4) abort_user(regs);

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
    serial_write_string(user_mode ? "User Mode (Ring 3)" : "Kernel Mode (Ring 0)");
    if (err & 0x08) serial_write_string("\n  [!] Reserved bit set dalam page table!");
    if (err & 0x10) serial_write_string("\n  [!] Instruction Fetch (NX violation)");

    /* ==================== KERNEL MODE FAULT: PANIC ==================== */
    serial_write_string("\n  [FATAL] Page fault kernel atau tabel rusak! CPU dihentikan.\n");
    serial_write_string("=============================================\n");

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
    fb_print((err & 0x01) ? "Penyebab: Protection Violation"
                          : "Penyebab: Page Not Present", 50, y, 0xFFFF00, 0x002244, 2);
    y += 22;
    fb_print((err & 0x02) ? "Operasi:  WRITE (Tulis)"
                          : "Operasi:  READ (Baca)", 50, y, 0xFFFF00, 0x002244, 2);
    y += 22;
    fb_print("Konteks:  Kernel Mode (Ring 0) — FATAL!", 50, y, 0xFF0000, 0x002244, 2);
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
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
    print_hex_serial("RFLAGS                ", regs->rflags);
    print_hex_serial("vector                ", regs->vector);

    /* Dump all general-purpose registers for full context */
    print_hex_serial("RAX", regs->rax);
    print_hex_serial("RBX", regs->rbx);
    print_hex_serial("RCX", regs->rcx);
    print_hex_serial("RDX", regs->rdx);
    print_hex_serial("RDI", regs->rdi);
    print_hex_serial("RSI", regs->rsi);
    print_hex_serial("RBP", regs->rbp);

    /* Only read the saved frame. An arbitrary #GP need not have a previous
     * IRET frame above it; probing beyond it can fault inside diagnostics. */
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
 * handle_double_fault() - Handler khusus untuk Double Fault (#DF, INT 8).
 *
 * Double Fault terjadi saat CPU menemui exception saat mencoba memanggil
 * exception handler sebelumnya — paling sering karena kernel stack overflow
 * atau corrupt. Tanpa IST, #DF mencoba pakai stack yang sama yang sudah
 * rusak, lalu Triple Fault (reboot diam-diameter, tanpa diagnostik).
 *
 * Berkat IST1 (di-set di kernel.c), CPU memuat RSP dari TSS.ist1 yang masih
 * utuh saat #DF fire, sehingga handler ini bisa berjalan dan mencetak
 * diagnostik. Karena ini kondisi fatal yang tidak bisa di-recover, handler
 * menghentikan CPU.
 *
 * Error code untuk #DF selalu 0 (zero error code), jadi tidak banyak info
 * tambahan — RIP dan RSP dari regs adalah petunjuk utama.
 */
static void handle_double_fault(struct registers *regs) {
    serial_write_string("\n========== DOUBLE FAULT (#DF, INT 8) ==========\n");
    serial_write_string("  Penyebab kemungkinan: kernel stack overflow/corrupt\n");
    print_hex_serial("RIP (Instruction)  ", regs->rip);
    print_hex_serial("RSP (Stack Pointer)", regs->rsp);
    print_hex_serial("CS  (Code Segment) ", regs->cs);
    print_hex_serial("SS  (Stack Segment)", regs->ss);
    print_hex_serial("RFLAGS             ", regs->rflags);
    print_hex_serial("Error Code         ", regs->err_code);
    serial_write_string("  Current PID: ");
    char pid_buf[16];
    itoa(get_current_pid(), pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string("\n================================================\n");

    /* Output ke layar */
    size_t y = 200;
    fb_print("======== DOUBLE FAULT (KERNEL STACK OVERFLOW?) ========", 50, y, 0xFF0000, 0x002244, 2);
    y += 28;
    print_hex_fb("RIP: ", regs->rip, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("RSP: ", regs->rsp, 50, y, 0xFF8800);
    y += 24;
    print_hex_fb("PID: ", (uint64_t)get_current_pid(), 50, y, 0xFF8800);
    y += 24;
    fb_print("Sistem dihentikan. Lihat serial log untuk detail.", 50, y, 0xFFFF00, 0x002244, 2);

    asm volatile ("cli");
    for (;;) { asm volatile ("hlt"); }
}

/*
 * isr_handler() - Dispatcher utama interrupt & exception.
 *
 * Fungsi ini dipanggil oleh isr_common (assembly) setiap kali CPU menerima
 * interrupt. Berdasarkan nomor interrupt, fungsi ini mengarahkan ke:
 *
 *   INT 0-31: User exception → stop group; kernel/platform exception → panic
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
    task_context_enter();
    if (regs->vector < 32) {
        /* ===== CPU EXCEPTION ===== */

        /* INT 14: Page Fault — may be recoverable (CoW or user fault) */
        if (regs->vector == 14) {
            handle_page_fault(regs);
            /* If handle_page_fault returns, it was either:
             * - CoW resolved or user copy recovered
             * - User address-space group killed (does not return)
             * - Kernel fault (cli+hlt inside handler, won't reach here)
             * Recoverable faults resume at the saved/fixed-up RIP. */
            return;
        }

        /* INT 8: Double Fault — punya IST1 dedicated stack (di-set di kernel.c) */
        if (regs->vector == 8) {
            handle_double_fault(regs);
            /* handle_double_fault tidak kembali (cli; hlt loop) */
        }

        if ((regs->cs & 3) == 3 && user_exception(regs->vector) &&
            percpu_current()->current && percpu_current()->current->pml4)
            abort_user(regs);

        /* Unrecoverable kernel/platform exceptions remain fatal. */
        serial_write_string("\n[!!!] KERNEL PANIC: ");
        serial_write_string(exception_messages[regs->vector]);
        serial_write_string(" [!!!]\n");

        if (regs->vector == 13) {
            /* INT 13: General Protection Fault */
            handle_gpf(regs);
        } else {
            /* Exception lainnya: tampilkan info dasar */
            fb_print("=============================================", 50, 300, 0xFF0000, 0x002244, 2);
            fb_print("                KERNEL PANIC!                ", 50, 320, 0xFF0000, 0x002244, 2);
            fb_print("=============================================", 50, 340, 0xFF0000, 0x002244, 2);
            fb_print(exception_messages[regs->vector], 50, 410, 0xFFFF00, 0x002244, 2);

            print_hex_fb("RIP: ", regs->rip, 50, 440, 0xFF8800);
            print_hex_fb("ERR: ", regs->err_code, 50, 464, 0xFF8800);

            print_hex_serial("RIP", regs->rip);
            print_hex_serial("Error Code", regs->err_code);
        }

        asm volatile ("cli");
        for (;;) { asm volatile ("hlt"); }
    }
    else if (regs->vector >= 32 && regs->vector <= 47) {
        /* ===== HARDWARE IRQ ===== */
        uint8_t irq = regs->vector - 32;
        if (!pic_begin_irq(irq)) return;

        /* PIT on BSP is the sole wall-clock and scheduler tick source. */
        if (regs->vector == 32) {
            timer_tick();
            schedule(regs);
        }
        /* IRQ 1 (INT 33): Keyboard PS/2 — input pengguna */
        else if (regs->vector == 33 || regs->vector == 44) {
            ps2_irq();
        }
        else virtio_irq_dispatch(irq);

        /* Kirim EOI (End of Interrupt) ke PIC */
        pic_end_irq(irq);
    }
    /* AP scheduler ticks are local; only BSP PIT advances global time. */
    else if (regs->vector == 48) {
        extern void lapic_eoi(void);
        lapic_eoi();
        if (smp_get_current_cpu_id() != 0) schedule(regs);
    } else if (regs->vector == 49) {
        tlb_poll();
        lapic_eoi();
    } else if (regs->vector == 50) {
        lapic_eoi();
        /* Periodic timer accounts for quantum; IPI only wakes idle CPUs. */
    } /* APIC spurious vector 255 needs no EOI. */
}

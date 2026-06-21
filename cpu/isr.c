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

#include "../mm/pmm.h"
#include "../mm/vmm.h"

/*
 * cow_resolve() - Handle Copy-on-Write page fault.
 *
 * When fork() marks pages as read-only + CoW (bit 9), any write triggers
 * a page fault. This function:
 *   1. Checks if the faulting PTE has CoW bit set
 *   2. If refcount == 1, just make it writable again (last owner)
 *   3. If refcount > 1, allocate new page, copy, remap writable
 *
 * Returns 1 if CoW was handled (resume execution), 0 if not a CoW fault.
 */
static int cow_resolve(uint64_t fault_addr, uint64_t err_code) {
    /* CoW only applies to WRITE faults on PRESENT pages from USER mode */
    if (!(err_code & 0x01)) return 0; /* Page not present → not CoW */
    if (!(err_code & 0x02)) return 0; /* Read fault → not CoW */
    if (!(err_code & 0x04)) return 0; /* Kernel mode → not CoW */

    /* Get current task's PML4 */
    uint64_t* pml4 = task_get_current_pml4();
    if (!pml4) return 0;

    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t offset = hhdm_request.response->offset;

    /* Walk page tables to find the PTE */
    uint64_t pml4_idx = (fault_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (fault_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (fault_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (fault_addr >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)((pml4[pml4_idx] & ~0xFFF) + offset);

    if (!(pdpt[pdpt_idx] & 1)) return 0;
    uint64_t* pd = (uint64_t*)((pdpt[pdpt_idx] & ~0xFFF) + offset);

    if (!(pd[pd_idx] & 1)) return 0;
    uint64_t* pt = (uint64_t*)((pd[pd_idx] & ~0xFFF) + offset);

    uint64_t pte = pt[pt_idx];
    if (!(pte & 1)) return 0;      /* Not present */
    if (!(pte & 0x200)) return 0;  /* No CoW bit (bit 9) → not our fault */

    uint64_t old_phys = pte & ~0xFFF;
    uint64_t flags = pte & 0xFFF;

    /* Remove CoW bit, add WRITABLE */
    flags = (flags & ~0x200) | 0x002;

    uint8_t refcount = pmm_get_refcount((void*)old_phys);

    if (refcount <= 1) {
        /* Last owner: just make writable, no copy needed */
        pt[pt_idx] = old_phys | flags;
    } else {
        /* Shared: allocate new page and copy */
        void* new_phys = pmm_alloc_page();
        if (!new_phys) {
            serial_write_string("[CoW] Out of memory!\n");
            return 0; /* Let it crash — no memory */
        }

        /* Copy 4KB content */
        uint8_t* src = (uint8_t*)(old_phys + offset);
        uint8_t* dst = (uint8_t*)((uint64_t)new_phys + offset);
        for (int i = 0; i < 4096; i++) dst[i] = src[i];

        /* Remap to new page (writable, no CoW) */
        pt[pt_idx] = (uint64_t)new_phys | flags;

        /* Decrement refcount of old shared page */
        pmm_free_page((void*)old_phys);

        /* Track new page in current task */
        struct task* t = task_find_by_pid(get_current_pid());
        if (t && t->user_page_count < 256) {
            t->user_pages[t->user_page_count++] = (uint64_t)new_phys;
        }
    }

    /* Flush TLB for this address */
    asm volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
    return 1; /* CoW handled, resume execution */
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
    if (cow_resolve(cr2, err)) {
        return; /* CoW handled — resume user code */
    }

    int user_mode = (regs->cs & 3) == 3; /* Ring 3 = user mode */

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

    if (user_mode) {
        /* ==================== USER MODE FAULT: KILL PROCESS ==================== */
        serial_write_string("\n  [ACTION] Membunuh proses user (PID ");
        char pid_buf[16];
        itoa(get_current_pid(), pid_buf, 10);
        serial_write_string(pid_buf);
        serial_write_string(")\n=============================================\n");

        /* ==================== OUTPUT KE LAYAR ==================== */
        size_t y = 300;
        fb_print("============= PAGE FAULT =============", 50, y, 0xFF4444, 0x002244, 2);
        y += 28;
        print_hex_fb("Alamat Fault (CR2): ", cr2, 50, y, 0xFF8800);
        y += 24;
        print_hex_fb("Instruksi (RIP):    ", regs->rip, 50, y, 0xFF8800);
        y += 24;
        fb_print("Proses user dibunuh (PID ", 50, y, 0xFFFF00, 0x002244, 2);
        fb_print(pid_buf, 50 + 25 * 16, y, 0xFFFF00, 0x002244, 2);

        /* Tandai task sebagai mati — reaper akan bersihkan di tick berikutnya */
        exit_current_task(); /* Tidak akan kembali: masuk loop hlt */
    } else {
        /* ==================== KERNEL MODE FAULT: PANIC ==================== */
        serial_write_string("\n  [FATAL] Page fault di kernel! Sistem dihentikan.\n");
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
    }
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
    print_hex_serial("int_no                ", regs->int_no);

    /* Dump all general-purpose registers for full context */
    print_hex_serial("RAX", regs->rax);
    print_hex_serial("RBX", regs->rbx);
    print_hex_serial("RCX", regs->rcx);
    print_hex_serial("RDX", regs->rdx);
    print_hex_serial("RDI", regs->rdi);
    print_hex_serial("RSI", regs->rsi);
    print_hex_serial("RBP", regs->rbp);

    /* Dump the ORIGINAL iretq frame that caused the GPF.
     * When GPF fires at iretq, the CPU pushes a new exception frame
     * ON TOP of the original frame. The original is at:
     *   regs + sizeof(struct registers) = regs + 22*8 = regs + 176
     * struct registers has 22 uint64_t fields (15 GPRs + int_no + err_code + 5 CPU state)
     */
    serial_write_string("\n  [Original iretq frame that faulted]:\n");
    uint64_t* orig = (uint64_t*)((uint8_t*)regs + 176);
    print_hex_serial("  orig RIP", orig[0]);
    print_hex_serial("  orig CS ", orig[1]);
    print_hex_serial("  orig RFL", orig[2]);
    print_hex_serial("  orig RSP", orig[3]);
    print_hex_serial("  orig SS ", orig[4]);

    /* Also dump the current GPF handler's own frame for reference */
    serial_write_string("  [GPF handler's own frame]:\n");
    uint64_t* self = (uint64_t*)((uint8_t*)regs + 160);
    print_hex_serial("  self RIP", self[0]);
    print_hex_serial("  self CS ", self[1]);
    print_hex_serial("  self RFL", self[2]);

    /* Dump current task PID for context */
    serial_write_string("  Current PID: ");
    char pid_buf[16];
    itoa(get_current_pid(), pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string("\n");

    /* Dump raw stack: show 30 entries (240 bytes) covering both frames */
    serial_write_string("  [Raw stack dump]:\n");
    uint64_t* stk = (uint64_t*)regs;
    for (int i = 0; i < 30; i++) {
        serial_write_string("    +");
        char off[8];
        itoa(i * 8, off, 10);
        serial_write_string(off);
        serial_write_string(": ");
        char val[20];
        itoa(stk[i], val, 16);
        serial_write_string(val);
        serial_write_string("\n");
    }

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
        /* ===== CPU EXCEPTION ===== */

        /* INT 14: Page Fault — may be recoverable (CoW or user fault) */
        if (regs->int_no == 14) {
            handle_page_fault(regs);
            /* If handle_page_fault returns, it was either:
             * - CoW resolved (resume user code)
             * - User process killed (exit_current_task loops, won't reach here)
             * - Kernel fault (cli+hlt inside handler, won't reach here)
             * So if we reach here, it was a CoW success — just return. */
            return;
        }

        /* All other exceptions are fatal */
        serial_write_string("\n[!!!] KERNEL PANIC: ");
        serial_write_string(exception_messages[regs->int_no]);
        serial_write_string(" [!!!]\n");

        if (regs->int_no == 13) {
            /* INT 13: General Protection Fault */
            handle_gpf(regs);
        } else {
            /* Exception lainnya: tampilkan info dasar */
            fb_print("=============================================", 50, 300, 0xFF0000, 0x002244, 2);
            fb_print("                KERNEL PANIC!                ", 50, 320, 0xFF0000, 0x002244, 2);
            fb_print("=============================================", 50, 340, 0xFF0000, 0x002244, 2);
            fb_print(exception_messages[regs->int_no], 50, 410, 0xFFFF00, 0x002244, 2);

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
    /* ===== LAPIC TIMER (INT 48) — SMP per-CPU scheduler tick ===== */
    else if (regs->int_no == 48) {
        extern void lapic_eoi(void);
        timer_tick();     /* Increment global tick counter */
        schedule(regs);   /* Context switch on this CPU */
        lapic_eoi();      /* Acknowledge LAPIC interrupt */
    }
}
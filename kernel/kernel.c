#include <stdint.h>
#include <stddef.h>
#include "../limine.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/timer.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "utils.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/heap.h"
#include "task.h"
#include "../cpu/syscall.h"
#include "../cpu/smp.h"
#include "../cpu/percpu.h"
#include "../fs/tar.h"
#include "../crypto/random.h"
#include "../security/cred.h"

LIMINE_BASE_REVISION(1)

/* ====================================================================
 * LIMINE v5.x REQUEST MARKERS (WAJIB untuk bare metal!)
 *
 * Limine v5.x memindai ELF untuk marker start & end yang membungkus
 * section .requests. Tanpa markers ini, bootloader mengabaikan semua
 * request → semua response NULL → kernel hang di hcf().
 *
 * Marker harus ditempatkan DI LUAR section .requests (di sectionnya
 * sendiri) agar Limine bisa menemukan batas awal/akhir blok request.
 * ==================================================================== */
__attribute__((used, section(".requests_start_marker")))
volatile uint64_t limine_requests_start_marker[4] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9
};

__attribute__((used, section(".requests_end_marker")))
volatile uint64_t limine_requests_end_marker[2] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62
};

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0 };

__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_request = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };

__attribute__((used, section(".requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST, .revision = 0, .flags = 0
};

static void hcf(void) { asm ("cli"); for (;;) { asm ("hlt"); } }

/*
 * Framebuffer global — digunakan oleh syscall handler (screen_clear, dll).
 * Dideklarasikan di sini karena framebuffer_request response hanya tersedia
 * di kernel.c setelah boot.
 */
struct limine_framebuffer *fb;

/*
 * _start() — Entry point kernel GenOS v3.
 *
 * Urutan inisialisasi:
 *   1. Serial (untuk debug output)
 *   2. GDT + TSS (segment descriptors Ring 0 & Ring 3)
 *   3. IDT + ISR (exception & interrupt handlers)
 *   4. PIC (hardware interrupt routing)
 *   5. PIT Timer (scheduler heartbeat @ 1000 Hz)
 *   6. Framebuffer (layar)
 *   7. PMM → VMM → Heap (memory management)
 *   8. Task Scheduler (multitasking engine)
 *   9. TAR Ramdisk (file system)
 *  10. Syscall Gateway (Ring 3 ↔ Ring 0 communication)
 *  11. Launch shell.elf (user-space terminal)
 *  12. Enable interrupts → idle loop
 *
 * PENTING: shell TIDAK LAGI berjalan di kernel (Ring 0).
 * Shell sekarang adalah ELF user-space di Ring 3 yang berkomunikasi
 * dengan kernel sepenuhnya melalui system call.
 */
/*
 * Boot kernel stack — 16KB (was 8KB).
 *
 * BUG FIX: 8KB was too small for the crypto/security init call chain:
 *   _start → cred_init → PBKDF2 → HMAC → SHA256 (~750 bytes deep)
 * Combined with all other boot initialization, 8KB overflows on bare
 * metal, corrupting the iretq frame and causing restart.
 */
uint8_t __attribute__((aligned(16))) boot_kernel_stack[16384];

void _start(void) {
    /*
     * BUG FIX: GUARANTEE interrupts are disabled from the very start.
     * On bare metal, the BIOS/UEFI may leave IF=1. Without this CLI,
     * a pending PIT/keyboard IRQ could fire before IDT is set up → #GP
     * or before PMM/VMM/Heap/Task are initialized → NULL dereference.
     * QEMU rarely triggers this race, but real hardware does.
     */
    asm volatile ("cli");

    if (LIMINE_BASE_REVISION_SUPPORTED == 0) hcf();

    /* === FASE 1: Hardware Dasar === */
    serial_init();
    serial_write_string("[INFO] Booting GenOS v3...\n");

    gdt_init();

    /*
     * BUG FIX: Initialize TSS.RSP0 with a temporary boot stack BEFORE
     * IDT is loaded. If any exception fires (e.g. #PF during PMM init),
     * the CPU needs a valid RSP0 to switch to Ring 0 stack.
     * Without this, TSS.RSP0 = 0 → CPU writes to address 0 → immediate
     * crash on bare metal (QEMU silently maps address 0).
     */
    set_kernel_stack((uint64_t)(boot_kernel_stack + sizeof(boot_kernel_stack)));

    idt_init();
    pic_remap();

    /*
     * BUG FIX: Do NOT call timer_init() here!
     * Timer PIT fires IRQ0 → schedule() → but task_init/heap not ready yet.
     * timer_init() is moved to AFTER all subsystems are initialized.
     */

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) hcf();
    if (memmap_request.response == NULL) hcf();
    fb = framebuffer_request.response->framebuffers[0];
    fb_init(fb);

    /*
     * === FASE 1.5: CPU Security Hardening ===
     *
     * Enable hardware security features SEBELUM user-space berjalan.
     * Fitur ini mencegah kelas serangan yang paling umum pada kernel.
     */
    {
        /*
         * SMEP (Supervisor Mode Execution Prevention) — CR4 bit 20
         *
         * Jika aktif, CPU akan #PF saat kernel mencoba EXECUTE kode di
         * page yang bertanda User (Ring 3). Ini memblokir serangan
         * ret2usr: attacker meletakkan shellcode di user-space, lalu
         * mengalihkan kernel RIP ke sana → SMEP memicu page fault.
         *
         * Cek CPUID leaf 7, ECX=0: EBX bit 7 = SMEP supported.
         */
        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));

        if (ebx & (1 << 7)) {  /* SMEP supported */
            uint64_t cr4;
            asm volatile("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1ULL << 20);  /* Set SMEP bit */
            asm volatile("mov %0, %%cr4" : : "r"(cr4));
            serial_write_string("[SEC] SMEP enabled (kernel cannot exec user pages)\n");
        } else {
            serial_write_string("[SEC] SMEP not supported by CPU\n");
        }

        /*
         * NXE (No-Execute Enable) — EFER MSR bit 11
         *
         * Mengaktifkan NX bit (bit 63) pada Page Table Entries.
         * Setelah diaktifkan, page yang ditandai NX=1 tidak bisa
         * dieksekusi → memungkinkan W^X (Write XOR Execute) policy:
         *   - Stack: writable, NOT executable
         *   - Heap: writable, NOT executable
         *   - Code: executable, NOT writable
         *
         * Ini mencegah shellcode injection di stack/heap.
         *
         * Cek CPUID extended leaf 0x80000001: EDX bit 20 = NX supported.
         */
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001), "c"(0));

        if (edx & (1 << 20)) {  /* NX supported */
            uint32_t efer_lo, efer_hi;
            asm volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080));
            efer_lo |= (1 << 11);  /* Set NXE bit */
            asm volatile("wrmsr" : : "a"(efer_lo), "d"(efer_hi), "c"(0xC0000080));
            serial_write_string("[SEC] NXE enabled (NX bit active in page tables)\n");
        } else {
            serial_write_string("[SEC] NX not supported by CPU\n");
        }
    }

    /* === FASE 2: Memory Management === */
    serial_write_string("[INFO] Initializing Memory Management...\n");
    pmm_init();
    vmm_init();
    heap_init();

    /* === FASE 2.5: SMP/CPU Detection via ACPI MADT === */
    smp_init(
        smp_request.response,
        rsdp_request.response ? rsdp_request.response->address : (void*)0
    );

    /*
     * === FASE 2.6: IST1 Stack (Double-Fault Safety Net) ===
     *
     * Alokasi dedicated stack untuk Double Fault (#DF) handler di tiap CPU.
     * Tanpa IST, kernel stack overflow → #DF mencoba pakai stack yang sama
     * yang sudah rusak → triple fault (reboot diam-diameter). Dengan IST1,
     * CPU switch ke stack ini yang masih utuh, sehingga handler bisa cetak
     * diagnostik ke layar + serial.
     *
     * 2 page (8 KB) per CPU cukup — handler double fault sengaja dibuat
     * minimalis (tidak ada call chain dalam).
     *
     * HARUS setelah smp_init (tahu jumlah CPU) dan pmm_init (bisa alokasi),
     * dan SEBELUM sti (interrupt di-enable).
     */
    {
        extern volatile struct limine_hhdm_request hhdm_request;
        uint64_t hhdm_off = hhdm_request.response->offset;
        uint32_t ncpus = smp_get_cpu_count();
        if (ncpus == 0) ncpus = 1;

        for (uint32_t c = 0; c < ncpus; c++) {
            uint64_t ist_phys = (uint64_t)pmm_alloc_contiguous_pages(2);
            if (ist_phys) {
                /* Stack grows down: top = base + 8KB (via HHDM) */
                uint64_t ist_top = ist_phys + hhdm_off + (2 * 4096);
                if (c == 0) {
                    gdt_set_ist1(ist_top);  /* BSP */
                } else {
                    percpu_set_ist1(c, ist_top);  /* AP */
                }
            } else {
                serial_write_string("[WARN] Cannot alloc IST1 stack for CPU\n");
            }
        }
        serial_write_string("[OK] IST1 (double-fault) stacks allocated\n");
    }

    /* === FASE 3: Task Scheduler === */
    serial_write_string("[INFO] Starting Task Scheduler...\n");
    task_init();

    /* === FASE 4: Ramdisk & Filesystem === */
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        serial_write_string("[INFO] Loading ramdisk module for TAR filesystem...\n");
        tar_init(module_request.response->modules[0]->address);
    } else {
        serial_write_string("[WARN] No ramdisk module; TAR filesystem disabled.\n");
    }

    /* === FASE 5: Cryptography & Security === */
    random_init();
    cred_init();

    /* === FASE 6: Syscall Gateway === */
    syscall_init();

    /* === FASE 6: Launch User-Space Shell ===
     *
     * Shell berjalan di Ring 3 (User Mode) sebagai ELF terpisah.
     * Semua akses ke hardware (layar, keyboard, filesystem) dilakukan
     * melalui system call. Jika shell crash, kernel tetap aman.
     *
     * shell.elf di-link di alamat 0x50000000.
     */
    serial_write_string("[INFO] Launching Security Shell (Ring 3)...\n");
    {
        size_t ukuran = 0;
        char* shell_data = tar_read_file("shell.elf", &ukuran);
        if (shell_data != NULL) {
            create_user_task((uint8_t*)shell_data);
            serial_write_string("[OK] Shell launched in Ring 3!\n");
        } else {
            serial_write_string("[FATAL] shell.elf not found in ramdisk!\n");
            fb_print("FATAL: shell.elf not found!", 50, 100, 0xFF0000, 0x002244, 2);
            hcf();
        }
    }

    /* === FASE 7: Aktifkan Timer & Interrupt → Idle Loop ===
     *
     * BUG FIX (Bare Metal): Timer PIT + LAPIC diinisialisasi di sini,
     * SETELAH semua subsistem (PMM, VMM, Heap, Task, Syscall, Shell)
     * siap. Ini mencegah IRQ menyala saat scheduler/heap belum aktif.
     *
     * smp_start_timers() juga melepas AP gate — APs mulai scheduling.
     */
    timer_init(1000);
    smp_start_timers(); /* Start BSP LAPIC timer + release AP gate */
    asm volatile ("sti");
    serial_write_string("[INFO] Kernel idle. All user work runs in Ring 3.\n");
    while (1) { asm volatile ("hlt"); }
}
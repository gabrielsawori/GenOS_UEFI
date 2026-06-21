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
#include "../fs/tar.h"

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
 * Temporary boot stack for TSS.RSP0 initialization.
 * Used before heap is available. After syscall_init(), kernel_stack_top
 * from heap replaces this. Aligned to 16 bytes per AMD64 ABI.
 */
uint8_t __attribute__((aligned(16))) boot_kernel_stack[8192];

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

    /* === FASE 2: Memory Management === */
    serial_write_string("[INFO] Initializing Memory Management...\n");
    pmm_init();
    vmm_init();
    heap_init();

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

    /* === FASE 5: Syscall Gateway === */
    syscall_init();

    /* === FASE 6: Launch User-Space Shell ===
     *
     * Shell berjalan di Ring 3 (User Mode) sebagai ELF terpisah.
     * Semua akses ke hardware (layar, keyboard, filesystem) dilakukan
     * melalui system call. Jika shell crash, kernel tetap aman.
     *
     * shell.elf di-link di alamat 0x50000000 (terpisah dari app.elf
     * yang di 0x40000000) agar keduanya bisa berjalan bersamaan.
     */
    serial_write_string("[INFO] Launching user-space shell (Ring 3)...\n");
    {
        size_t ukuran = 0;
        char* shell_data = tar_read_file("shell.elf", &ukuran);
        if (shell_data != NULL) {
            create_user_task((uint8_t*)shell_data);
            serial_write_string("[OK] Shell launched in Ring 3!\n");
        } else {
            serial_write_string("[FATAL] shell.elf not found in ramdisk!\n");
            /* Tampilkan pesan darurat di layar */
            fb_print("FATAL: shell.elf not found!", 50, 100, 0xFF0000, 0x002244, 2);
            fb_print("Ramdisk must contain shell.elf", 50, 130, 0xFFFF00, 0x002244, 2);
            hcf();
        }
    }

    /* === FASE 7: Aktifkan Timer & Interrupt → Idle Loop ===
     *
     * BUG FIX: Timer PIT diinisialisasi di sini, SETELAH semua subsistem
     * (PMM, VMM, Heap, Task, Syscall) siap. Ini mencegah IRQ0 menyala
     * saat scheduler/heap belum aktif — penyebab utama crash bare metal.
     *
     * Urutan HARUS: timer_init() → sti. Timer hanya mulai generate IRQ
     * setelah PIT diprogram, dan interrupt hanya diterima setelah sti.
     */
    timer_init(1000);
    asm volatile ("sti");
    serial_write_string("[INFO] Kernel idle. All user work runs in Ring 3.\n");
    while (1) { asm volatile ("hlt"); }
}
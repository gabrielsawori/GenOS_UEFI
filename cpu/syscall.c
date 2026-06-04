#include "syscall.h"
#include "msr.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../mm/heap.h"
#include "../kernel/task.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);

uint64_t kernel_stack_top = 0;

void syscall_init(void) {
    serial_write_string("[INFO] Membangun Pintu Gerbang System Call...\n");

    uint64_t stack_base = (uint64_t)kmalloc(4096);
    kernel_stack_top = stack_base + 4096;
    set_kernel_stack(kernel_stack_top);

    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1); // Enable syscall/sysret

    /*
     * STAR MSR Layout:
     *   STAR[47:32] = Kernel CS selector (untuk SYSCALL masuk kernel)
     *     → CPU otomatis: Kernel CS = STAR[47:32],  Kernel SS = STAR[47:32] + 8
     *   STAR[63:48] = Base selector (untuk SYSRETQ kembali ke user)
     *     → CPU otomatis: User CS = (STAR[63:48] + 16) | 3
     *                     User SS = (STAR[63:48] + 8)  | 3
     *
     * GDT kita:
     *   [0]=Null  [1]=KCode(0x08)  [2]=KData(0x10)
     *   [3]=UData(0x18/0x1B)  [4]=UCode(0x20/0x23)  [5-6]=TSS(0x28)
     *
     * Maka STAR[63:48] = 0x10 agar:
     *   User CS = (0x10 + 16) | 3 = 0x20 | 3 = 0x23 ✓ (GDT[4] User Code)
     *   User SS = (0x10 + 8)  | 3 = 0x18 | 3 = 0x1B ✓ (GDT[3] User Data)
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200); // Clear IF (interrupt flag) saat syscall

    serial_write_string("[OK] Pintu Gerbang Syscall Berhasil Dibuka!\n");
    serial_write_string("     Syscall tersedia: 1=print, 2=exit, 3=read_key, 4=sleep\n");
}

/*
 * syscall_handler() - Dispatcher utama System Call dari Ring 3.
 *
 * Dipanggil oleh syscall_entry (assembly) setelah argumen diterjemahkan
 * dari konvensi syscall ke konvensi C (System V AMD64 ABI).
 *
 * Tabel Syscall GenOS:
 * ┌──────┬────────────┬─────────────────────┬───────────┬──────────────┐
 * │  No  │  Nama      │  arg1 (RSI→RDI)     │ arg2      │ Return (RAX) │
 * ├──────┼────────────┼─────────────────────┼───────────┼──────────────┤
 * │  1   │  print     │  char* teks         │ uint y_pos│ 0            │
 * │  2   │  exit      │  int status         │ -         │ (tidak kembali)│
 * │  3   │  read_key  │  -                  │ -         │ char (0=kosong)│
 * │  4   │  sleep     │  uint32 milidetik   │ -         │ 0            │
 * └──────┴────────────┴─────────────────────┴───────────┴──────────────┘
 *
 * @param syscall_num: nomor syscall (dari RAX user)
 * @param arg1: argumen pertama (dari RDI user)
 * @param arg2: argumen kedua (dari RSI user)
 * @param arg3: argumen ketiga (dari RDX user)
 * @return: nilai kembalian yang akan masuk ke RAX user
 */
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg3;

    switch (syscall_num) {

    /*
     * SYSCALL 1: print(char* text, uint64_t y_pos)
     *
     * Mencetak teks dari aplikasi Ring-3 ke layar framebuffer dan serial.
     * arg1 = pointer ke string (di memori user-space)
     * arg2 = koordinat Y pada layar (0 = default 100)
     */
    case 1: {
        char* pesan = (char*)arg1;
        uint64_t y_pos = arg2;
        if (y_pos == 0) y_pos = 100;
        serial_write_string("[APP] ");
        serial_write_string(pesan);
        serial_write_string("\n");
        fb_print("[APP RING-3] -> ", 50, y_pos, 0xFFFF00, 0x002244, 2);
        fb_print(pesan, 250, y_pos, 0xFFFFFF, 0x002244, 2);
        return 0;
    }

    /*
     * SYSCALL 2: exit(int status)
     *
     * Mengakhiri task yang sedang berjalan dengan aman.
     * Task ditandai TASK_DEAD dan scheduler akan melewatinya.
     * Fungsi ini TIDAK PERNAH kembali ke caller.
     */
    case 2:
        exit_current_task();
        return 0; /* Tidak pernah tercapai */

    /*
     * SYSCALL 3: read_key(void) -> char
     *
     * Membaca satu karakter dari ring buffer keyboard.
     * Non-blocking: mengembalikan 0 jika tidak ada tombol yang ditekan.
     * Aplikasi harus memanggil berulang (polling) atau kombinasi dengan sleep.
     *
     * Return: karakter ASCII (misal 'a', '1', '\n') atau 0 jika buffer kosong.
     */
    case 3: {
        char c = keyboard_get_char();
        return (uint64_t)c;
    }

    /*
     * SYSCALL 4: sleep(uint32_t milliseconds)
     *
     * Menahan eksekusi task selama N milidetik.
     * Menggunakan timer PIT yang berdetak 1000x/detik (1 tick = 1 ms).
     *
     * CATATAN: Selama sleep, CPU melakukan HLT di loop — interrupt (termasuk
     * timer) masih aktif sehingga scheduler tetap bisa context-switch ke task
     * lain. Namun task yang memanggil sleep akan tetap di dalam loop sampai
     * waktu habis. Ini adalah implementasi "busy-wait with HLT" yang sederhana.
     *
     * arg1 = jumlah milidetik untuk tidur
     */
    case 4: {
        uint64_t target = timer_get_ticks() + arg1;
        while (timer_get_ticks() < target) {
            asm volatile ("sti; hlt");
        }
        return 0;
    }

    default:
        serial_write_string("[WARN] Syscall tidak dikenal: ");
        serial_write_string("nomor tidak valid\n");
        return (uint64_t)-1;
    }
}
#include "syscall.h"
#include "msr.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../mm/heap.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../fs/tar.h"
#include "../fs/vfs.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);
extern struct limine_framebuffer *fb;

uint64_t kernel_stack_top = 0;

/*
 * Flag global: menandai bahwa CPU sedang di dalam syscall handler.
 * Scheduler HARUS skip context-switch saat flag ini aktif, karena
 * syscall menggunakan stack terpisah (kernel_stack_top) yang tidak
 * kompatibel dengan mekanisme iretq scheduler.
 */
volatile int in_syscall = 0;

void syscall_init(void) {
    serial_write_string("[INFO] Membangun Pintu Gerbang System Call...\n");

    uint64_t stack_base = (uint64_t)kmalloc(4096);
    kernel_stack_top = stack_base + 4096;
    set_kernel_stack(kernel_stack_top);

    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);

    /*
     * STAR MSR Layout:
     *   STAR[47:32] = Kernel CS (0x08)
     *   STAR[63:48] = Base untuk SYSRETQ (0x10)
     *     → User CS = (0x10 + 16) | 3 = 0x23 ✓
     *     → User SS = (0x10 + 8)  | 3 = 0x1B ✓
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);

    serial_write_string("[OK] Pintu Gerbang Syscall Berhasil Dibuka!\n");
    serial_write_string("     Syscall: 1=print 2=exit 3=read_key 4=sleep\n");
    serial_write_string("             5=clear 6=print_at 7=draw_char 8=read_file 9=exec 10=fill_rect 11=wait_pid\n");
    serial_write_string("            12=open 13=read 14=close 15=seek 16=readdir\n");

    /* Initialize VFS layer */
    vfs_init();
}

/*
 * syscall_handler() - Dispatcher utama System Call dari Ring 3.
 *
 * Tabel Syscall GenOS:
 * ┌────┬─────────────┬──────────────────────┬──────────────────┬──────────────┬──────────────┐
 * │ No │ Nama        │ arg1                 │ arg2             │ arg3         │ Return (RAX) │
 * ├────┼─────────────┼──────────────────────┼──────────────────┼──────────────┼──────────────┤
 * │  1 │ print       │ char* teks           │ uint y_pos       │ -            │ 0            │
 * │  2 │ exit        │ int status           │ -                │ -            │ (tak kembali)│
 * │  3 │ read_key    │ -                    │ -                │ -            │ char / 0     │
 * │  4 │ sleep       │ uint32 ms            │ -                │ -            │ 0            │
 * │  5 │ screen_clear│ -                    │ -                │ -            │ 0            │
 * │  6 │ print_at    │ char* teks           │ (x<<32)|y        │ fg_color     │ 0            │
 * │  7 │ draw_char   │ char c               │ (x<<32)|y        │ fg_color     │ 0            │
 * │  8 │ read_file   │ char* filename       │ char* buffer     │ max_size     │ bytes_read   │
 * │  9 │ exec        │ char* filename       │ -                │ -            │ pid / -1     │
 * │ 10 │ fill_rect   │ (x<<32)|y            │ (w<<32)|h        │ color        │ 0            │
 * │ 11 │ wait_pid    │ uint32 pid           │ -                │ -            │ 0=alive 1=dead│
 * └────┴─────────────┴──────────────────────┴──────────────────┴──────────────┴──────────────┘
 */
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    in_syscall = 1;
    /*
     * BUG FIX: Paksa task ke TASK_RUNNING sebelum memproses syscall.
     *
     * Jika task sebelumnya memanggil sleep (yang men-set TASK_SLEEPING),
     * lalu task kembali dan membuat syscall berikutnya (mis. wait_pid),
     * state task masih TASK_SLEEPING. Jika timer IRQ menyala saat task
     * di dalam syscall ini, scheduler akan melewati task (karena SLEEPING)
     * dan TIDAK menyimpan register ke task->regs. Akibatnya, state task
     * bisa korup saat dibangunkan nanti.
     *
     * Dengan men-set TASK_RUNNING di sini, scheduler selalu memperlakukan
     * task ini sebagai task aktif jika timer menyala di tengah syscall.
     */
    task_mark_running();
    /* Aktifkan interrupt agar keyboard & timer IRQ bisa menyala */
    asm volatile ("sti");

    uint64_t result = 0;
    switch (syscall_num) {

    /* ================================================================
     * SYSCALL 1: print(char* text, uint64_t y_pos)
     * Cetak teks ke framebuffer + serial (kompatibilitas lama)
     * ================================================================ */
    case 1: {
        char* pesan = (char*)arg1;
        uint64_t y_pos = arg2;
        if (y_pos == 0) y_pos = 100;
        serial_write_string("[APP] ");
        serial_write_string(pesan);
        serial_write_string("\n");
        fb_print("[APP RING-3] -> ", 50, y_pos, 0xFFFF00, 0x002244, 2);
        fb_print(pesan, 250, y_pos, 0xFFFFFF, 0x002244, 2);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 2: exit(int status)
     * Akhiri task yang sedang berjalan. Tidak pernah kembali.
     * ================================================================ */
    case 2:
        serial_write_string("[EXIT] Task called exit()\n");
        in_syscall = 0;
        exit_current_task();
        result = 0; break; /* tak pernah tercapai */

    /* ================================================================
     * SYSCALL 3: read_key(void) -> char
     * Baca 1 karakter dari keyboard buffer (non-blocking).
     * Return 0 jika buffer kosong.
     * ================================================================ */
    case 3: {
        char c = keyboard_get_char();
        result = (uint64_t)c;
        break;
    }

    /* ================================================================
     * SYSCALL 4: sleep(uint32_t milliseconds)
     *
     * BUG FIX: Versi lama menggunakan busy-wait (pause loop) dengan
     * in_syscall = 1 selama N milidetik. Ini MEMBLOKIR scheduler dari
     * context-switch karena scheduler selalu skip saat in_syscall aktif.
     *
     * Akibatnya: saat shell memanggil user_sleep(50) dalam polling loop
     * `while (!wait_pid(pid)) user_sleep(50)`, app task TIDAK PERNAH
     * mendapat giliran CPU — shell memonopoli CPU selama 50ms per iterasi,
     * lalu hanya memberi jendela ~50ns sebelum syscall berikutnya.
     *
     * PERBAIKAN: Gunakan sleep_current_task() untuk menandai task sebagai
     * TASK_SLEEPING. Setelah sysretq, timer IRQ berikutnya akan memanggil
     * schedule(), yang melihat task SLEEPING dan langsung switch ke task
     * lain (mis. app). Task dibangunkan otomatis saat timer tick >= target.
     * ================================================================ */
    case 4: {
        if (arg1 > 0) {
            sleep_current_task(timer_get_ticks() + arg1);
        }
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 5: screen_clear(void)
     * Bersihkan seluruh layar framebuffer dengan warna background.
     * ================================================================ */
    case 5: {
        for (size_t y = 0; y < fb->height; y++) {
            for (size_t x = 0; x < fb->width; x++) {
                fb_draw_pixel(x, y, 0x002244);
            }
        }
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 6: print_at(char* text, (x << 32) | y, fg_color)
     * Cetak teks pada posisi (x, y) dengan warna foreground tertentu.
     * Koordinat di-pack: arg2 = (x << 32) | (y & 0xFFFFFFFF)
     * ================================================================ */
    case 6: {
        char* text = (char*)arg1;
        uint32_t x = (uint32_t)(arg2 >> 32);
        uint32_t y = (uint32_t)(arg2 & 0xFFFFFFFF);
        uint32_t color = (uint32_t)arg3;
        fb_print(text, x, y, color, 0x002244, 2);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 7: draw_char(char c, (x << 32) | y, fg_color)
     * Gambar 1 karakter pada posisi (x, y) dengan warna tertentu.
     * ================================================================ */
    case 7: {
        char c = (char)arg1;
        uint32_t x = (uint32_t)(arg2 >> 32);
        uint32_t y = (uint32_t)(arg2 & 0xFFFFFFFF);
        uint32_t color = (uint32_t)arg3;
        fb_draw_char(c, x, y, color, 0x002244, 2);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 8: read_file(char* filename, char* buffer, uint64_t max)
     * Baca file dari TAR ramdisk ke buffer user-space.
     * Return: jumlah byte yang dibaca, atau 0 jika file tidak ditemukan.
     * ================================================================ */
    case 8: {
        char* filename = (char*)arg1;
        char* user_buf = (char*)arg2;
        uint64_t max_size = arg3;

        size_t file_size = 0;
        char* file_data = tar_read_file(filename, &file_size);
        if (file_data == NULL) return 0;

        /* Salin ke buffer user, batasi ukuran */
        uint64_t copy_size = file_size < max_size ? file_size : max_size;
        for (uint64_t i = 0; i < copy_size; i++) {
            user_buf[i] = file_data[i];
        }
        result = copy_size;
        break;
    }

    /* ================================================================
     * SYSCALL 9: exec(char* filename)
     * Muat dan jalankan file ELF dari ramdisk sebagai user task baru.
     *
     * BUG FIX #17: Mengembalikan PID child (>0) agar shell bisa menunggu
     * child selesai dengan syscall `wait_pid` (#11). Versi blocking
     * langsung di kernel TIDAK aman karena `kernel_stack_top` di-share
     * antar syscall — child yang melakukan syscall akan menimpa frame
     * kernel shell yang sedang block. Karena itu polling dilakukan di
     * Ring 3 dengan user_sleep, sehingga kernel stack hanya terpakai
     * sebentar saat tiap iterasi syscall.
     *
     * Return: PID child (>0) bila berhasil, atau (uint64_t)-1 bila gagal.
     * ================================================================ */
    case 9: {
        char* filename = (char*)arg1;
        size_t ukuran = 0;
        char* elf_data = tar_read_file(filename, &ukuran);
        if (elf_data == NULL) {
            serial_write_string("[EXEC] File not found: ");
            serial_write_string(filename);
            serial_write_string("\n");
            result = (uint64_t)-1;
            break;
        }

        struct task* child = create_user_task((uint8_t*)elf_data);
        if (child == NULL) { result = (uint64_t)-1; break; }

        serial_write_string("[EXEC] Launched PID ");
        char pid_buf[16];
        itoa(child->pid, pid_buf, 10);
        serial_write_string(pid_buf);
        serial_write_string("\n");
        result = (uint64_t)child->pid;
        break;
    }

    /* ================================================================
     * SYSCALL 10: fill_rect((x<<32)|y, (w<<32)|h, color)
     * Isi area persegi pada framebuffer dengan satu warna.
     *
     * Digunakan oleh shell/aplikasi Ring-3 untuk membersihkan baris
     * sebelum menulis ulang teks (mencegah tumpang tindih karakter).
     * Koordinat & ukuran di-pack ke 2 argumen 64-bit:
     *   arg1 = ((uint64_t)x << 32) | (y & 0xFFFFFFFF)
     *   arg2 = ((uint64_t)w << 32) | (h & 0xFFFFFFFF)
     * ================================================================ */
    case 10: {
        uint32_t x = (uint32_t)(arg1 >> 32);
        uint32_t y = (uint32_t)(arg1 & 0xFFFFFFFF);
        uint32_t w = (uint32_t)(arg2 >> 32);
        uint32_t h = (uint32_t)(arg2 & 0xFFFFFFFF);
        uint32_t color = (uint32_t)arg3;
        fb_fill_rect(x, y, w, h, color);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 11: wait_pid(uint32_t pid)
     * BLOKIR task saat ini sampai task target selesai (TASK_DEAD).
     *
     * Setelah wait_for_pid() men-set TASK_WAITING, scheduler akan
     * skip task ini dan memberi CPU penuh ke child. Saat child
     * memanggil exit, reaper membangunkan parent ke TASK_READY.
     * Tidak ada lagi polling busy-wait dari Ring 3.
     *
     * Return: 1 = task sudah selesai (atau tidak ditemukan).
     * ================================================================ */
    case 11: {
        uint32_t want_pid = (uint32_t)arg1;
        char buf[32];
        serial_write_string("[WAIT] PID ");
        itoa(want_pid, buf, 10);
        serial_write_string(buf);
        serial_write_string(" -> blocking...\n");
        wait_for_pid(want_pid);
        result = 1; /* Selalu 1 karena saat return, target sudah mati */
        break;
    }

    /* ================================================================
     * SYSCALL 12: open(char* filename, uint32_t flags)
     * Buka file atau direktori melalui VFS layer.
     * filename = "/" membuka listing direktori (semua entry TAR).
     * flags diabaikan untuk sekarang (TAR read-only).
     * Return: FD number (>=0) atau -1 bila gagal.
     * ================================================================ */
    case 12: {
        char* filename = (char*)arg1;
        uint32_t pid = get_current_pid();
        int fd = vfs_open(pid, filename);
        result = (uint64_t)(int64_t)fd;
        break;
    }

    /* ================================================================
     * SYSCALL 13: read(int fd, char* buf, size_t count)
     * Baca hingga count byte dari file descriptor ke buffer user.
     * Return: jumlah byte yang dibaca, 0 = EOF, -1 = error.
     * ================================================================ */
    case 13: {
        int fd = (int)(int64_t)arg1;
        char* buf = (char*)arg2;
        size_t count = (size_t)arg3;
        uint32_t pid = get_current_pid();
        int bytes = vfs_read(pid, fd, buf, count);
        result = (uint64_t)(int64_t)bytes;
        break;
    }

    /* ================================================================
     * SYSCALL 14: close(int fd)
     * Tutup file descriptor dan bebaskan slot FD.
     * Return: 0 = sukses, -1 = error.
     * ================================================================ */
    case 14: {
        int fd = (int)(int64_t)arg1;
        uint32_t pid = get_current_pid();
        int ret = vfs_close(pid, fd);
        result = (uint64_t)(int64_t)ret;
        break;
    }

    /* ================================================================
     * SYSCALL 15: seek(int fd, int64_t offset, int whence)
     * Ubah posisi baca file: 0=SET, 1=CUR, 2=END.
     * Return: posisi baru (>=0) atau -1 bila error.
     * ================================================================ */
    case 15: {
        int fd = (int)(int64_t)arg1;
        int64_t offset = (int64_t)arg2;
        int whence = (int)arg3;
        uint32_t pid = get_current_pid();
        int ret = vfs_seek(pid, fd, offset, whence);
        result = (uint64_t)(int64_t)ret;
        break;
    }

    /* ================================================================
     * SYSCALL 16: readdir(int fd, char* name_buf, size_t* size_buf)
     * Baca entry direktori berikutnya dari FD yang dibuka dengan "/".
     * name_buf diisi nama file, *size_buf diisi ukuran file.
     * Return: 1 = entry ditemukan, 0 = selesai (tidak ada entry lagi).
     * ================================================================ */
    case 16: {
        int fd = (int)(int64_t)arg1;
        char* name_buf = (char*)arg2;
        size_t* size_buf = (size_t*)arg3;
        uint32_t pid = get_current_pid();
        int ret = vfs_readdir(pid, fd, name_buf, size_buf);
        result = (uint64_t)ret;
        break;
    }

    default:
        serial_write_string("[WARN] Syscall tidak dikenal\n");
        result = (uint64_t)-1;
        break;
    }

    /* Matikan interrupt sebelum kembali ke syscall_entry (sysretq membutuhkan CLI) */
    asm volatile ("cli");
    in_syscall = 0;
    return result;
}
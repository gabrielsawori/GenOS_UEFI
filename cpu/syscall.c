#include "syscall.h"
#include "msr.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../fs/tar.h"
#include "../fs/vfs.h"
#include "../fs/cache.h"
#include "../mm/shm.h"

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

    /*
     * BUG FIX: Allocate syscall kernel stack via PMM (2 pages = 8KB)
     * instead of kmalloc(4096). This ensures:
     *   1. Larger stack for complex syscalls (fork, exec)
     *   2. Physical page alignment (no heap header misalignment)
     *   3. Consistent with per-task kernel stack scheme
     *
     * Note: This stack is used by syscall_entry (via kernel_stack_top)
     * and also set as TSS.RSP0. Per-task kernel stacks in schedule()
     * will override TSS.RSP0 during context switches.
     */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm_off = hhdm_request.response->offset;
    uint64_t phys1 = (uint64_t)pmm_alloc_page();
    uint64_t phys2 = (uint64_t)pmm_alloc_page();
    if (!phys1 || !phys2) {
        serial_write_string("[FATAL] Cannot allocate syscall kernel stack!\n");
        asm volatile ("cli");
        for (;;) asm volatile ("hlt");
    }
    /* Stack grows down: top = end of second page (via HHDM) */
    kernel_stack_top = phys2 + hhdm_off + 4096;
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
    serial_write_string("            17=shm_create 18=shm_attach 19=shm_detach 20=shm_destroy\n");
    serial_write_string("            21=cache_stats 22=fork\n");
    serial_write_string("            23=write 24=create 25=unlink\n");

    /* Initialize VFS layer */
    vfs_init();

    /* Initialize Shared Memory layer */
    shm_init();
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
     * flags: O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREATE=4
     * Return: FD number (>=0) atau -1 bila gagal.
     * ================================================================ */
    case 12: {
        char* filename = (char*)arg1;
        int flags = (int)(int64_t)arg2;
        uint32_t pid = get_current_pid();
        int fd = vfs_open(pid, filename, flags);
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

    /* ================================================================
     * SYSCALL 17: shm_create(uint32_t size)
     * Buat shared memory segment baru.
     * Return: shmid (>=0) atau -1 bila gagal.
     * ================================================================ */
    case 17: {
        uint32_t size = (uint32_t)arg1;
        uint32_t pid = get_current_pid();
        int shmid = shm_create(pid, size);
        result = (uint64_t)(int64_t)shmid;
        break;
    }

    /* ================================================================
     * SYSCALL 18: shm_attach(int shmid)
     * Pasang (map) shared memory segment ke address space proses ini.
     * Return: virtual address (>0) atau 0 bila gagal.
     * ================================================================ */
    case 18: {
        int shmid = (int)(int64_t)arg1;
        uint32_t pid = get_current_pid();
        uint64_t* pml4 = task_get_current_pml4();
        uint64_t vaddr = shm_attach(shmid, pid, pml4);
        result = vaddr;
        break;
    }

    /* ================================================================
     * SYSCALL 19: shm_detach(uint64_t addr)
     * Lepas (unmap) shared memory dari address space proses ini.
     * Return: 0 = sukses, -1 = error.
     * ================================================================ */
    case 19: {
        uint64_t addr = arg1;
        uint32_t pid = get_current_pid();
        uint64_t* pml4 = task_get_current_pml4();
        int ret = shm_detach(addr, pid, pml4);
        result = (uint64_t)(int64_t)ret;
        break;
    }

    /* ================================================================
     * SYSCALL 20: shm_destroy(int shmid)
     * Hancurkan shared memory segment (hanya jika tidak ada yang attach).
     * Return: 0 = sukses, -1 = error / masih ada yang attach.
     * ================================================================ */
    case 20: {
        int shmid = (int)(int64_t)arg1;
        int ret = shm_destroy(shmid);
        result = (uint64_t)(int64_t)ret;
        break;
    }

    /* ================================================================
     * SYSCALL 21: cache_stats(cache_stats_t* out)
     * Ambil statistik performa buffer cache.
     * arg1 = pointer ke struct cache_stats_t di user-space.
     * Return: 0 = sukses, -1 = error.
     * ================================================================ */
    case 21: {
        cache_stats_t* out = (cache_stats_t*)arg1;
        if (!out) { result = (uint64_t)-1; break; }
        cache_stats_t s = cache_get_stats();
        out->hits        = s.hits;
        out->misses      = s.misses;
        out->evictions   = s.evictions;
        out->used_blocks = s.used_blocks;
        out->total_blocks = s.total_blocks;
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 22: fork(void)
     * Kloning proses saat ini. Child mendapat salinan penuh address space,
     * register state, dan FD table parent.
     * Return: PID child (>0) ke parent, 0 ke child, -1 bila gagal.
     * ================================================================ */
    case 22: {
        int32_t child_pid = task_fork();
        result = (uint64_t)(int64_t)child_pid;
        break;
    }

    /* ================================================================
     * SYSCALL 23: write(int fd, void* buf, size_t count)
     * Write data to a file descriptor (must be writable/tmpfs).
     * Return: bytes written or -1 on error.
     * ================================================================ */
    case 23: {
        int fd = (int)(int64_t)arg1;
        void* buf = (void*)arg2;
        size_t count = (size_t)arg3;
        uint32_t pid = get_current_pid();
        int bytes = vfs_write(pid, fd, buf, count);
        result = (uint64_t)(int64_t)bytes;
        break;
    }

    /* ================================================================
     * SYSCALL 24: create(char* filename)
     * Create a new empty file in tmpfs.
     * Return: 0 on success, -1 on error.
     * ================================================================ */
    case 24: {
        char* filename = (char*)arg1;
        uint32_t pid = get_current_pid();
        int ret = vfs_create(pid, filename);
        result = (uint64_t)(int64_t)ret;
        break;
    }

    /* ================================================================
     * SYSCALL 25: unlink(char* filename)
     * Delete a file from tmpfs.
     * Return: 0 on success, -1 on error.
     * ================================================================ */
    case 25: {
        char* filename = (char*)arg1;
        uint32_t pid = get_current_pid();
        int ret = vfs_unlink(pid, filename);
        result = (uint64_t)(int64_t)ret;
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
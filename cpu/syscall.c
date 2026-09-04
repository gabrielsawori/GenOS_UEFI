#include "syscall.h"
#include "msr.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../kernel/task.h"
#include "../kernel/utils.h"
#include "../fs/tar.h"
#include "../fs/vfs.h"
#include "../fs/cache.h"
#include "../mm/shm.h"
#include "../crypto/sha256.h"
#include "../crypto/aes.h"
#include "../crypto/hmac.h"
#include "../crypto/random.h"
#include "../security/cred.h"
#include "../security/keystore.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);
extern struct limine_framebuffer *fb;
extern volatile struct limine_hhdm_request hhdm_request;

uint64_t kernel_stack_top = 0;

/*
 * Framebuffer back-buffer page tracking (separate from user_pages[]).
 * Tracked here to avoid bloating struct task (which caused kmalloc
 * failures and triple faults on bare metal). Only one back-buffer
 * can exist at a time. Pages are reused if map_framebuffer is called
 * again by the same or different process.
 */
/*
 * FB_MAX_PAGES: Maximum number of 4 KB pages we can allocate for the
 * framebuffer back-buffer.
 *
 * BUG FIX (Bare Metal): Previously 1024 (4 MB), which silently truncated
 * the buffer on screens larger than ~1366×768. On bare metal resolutions
 * like 1920×1080 (2025 pages) or 4K 3840×2160 (8100 pages), the truncated
 * buffer left the upper portion unmapped. Userspace (desktop.c) and
 * syscall 35 (flush_screen) then read/wrote unmapped memory, corrupting
 * the kernel iretq frame and causing a page-fault at a garbage RIP
 * (RIP==CR2, error 0x10, kernel mode) — exactly the crash users saw
 * when launching `desktop` on a real PC.
 *
 * 8192 pages = 32 MB, comfortably covers 4K UHD (8100 pages) with margin.
 * The fb_phys_pages[] array grows in BSS (8192 × 8 B = 64 KB), acceptable.
 */
#define FB_MAX_PAGES 8192
static uint64_t fb_phys_pages[FB_MAX_PAGES];
static uint32_t fb_phys_count = 0;
static uint64_t fb_mapped_va = 0;   /* 0 = not yet mapped */

/*
 * in_syscall flag: DEPRECATED for scheduling purposes.
 *
 * Previously this flag blocked ALL context switches during syscalls.
 * Now that each task has its own kernel stack (per-task stack scheme),
 * the scheduler can safely preempt during syscalls:
 *   - Timer interrupt pushes onto the same per-task kernel stack
 *   - Scheduler saves/restores the interrupt frame correctly
 *   - iretq returns to inside the syscall handler on the per-task stack
 *   - Syscall continues and sysretq returns to user mode
 *
 * This is exactly how Linux handles preemptible syscalls.
 * The flag is kept only for debugging/tracing purposes.
 */
volatile int in_syscall = 0;

/*
 * syscall_init_cpu() - Konfigurasi MSR syscall pada CPU *yang sedang berjalan*.
 *
 * MSRs (EFER, STAR, LSTAR, FMASK) bersifat PER-CPU — setiap core punya
 * salinannya sendiri. Memanggil wrmsr() hanya pada BSP TIDAK mengaktifkan
 * syscall pada Application Processor.
 *
 * Tanpa EFER.SCE=1 pada suatu core, instruksi 'syscall' (0F 05) di user-space
 * memicu #UD (Invalid Opcode). Pada bare metal multi-core, scheduler memigrasi
 * task dari BSP ke AP — bila AP belum meng-enable SCE, 'syscall' pertama di
 * AP langsung #UD → kernel panic "Invalid Opcode".
 *
 * Fungsi ini HARUS dipanggil pada setiap CPU (BSP di syscall_init(), dan
 * setiap AP di ap_entry()) sebelum interrupt/timer diaktifkan.
 */
void syscall_init_cpu(void) {
    /* EFER.SCE (bit 0) — enable SYSCALL/SYSRET instruction pair */
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
}

void syscall_init(void) {
    serial_write_string("[INFO] Membangun Pintu Gerbang System Call...\n");

    /*
     * BUG FIX: Allocate syscall kernel stack via PMM (4 pages = 16KB)
     * instead of kmalloc(4096). This ensures:
     *   1. Larger stack for complex syscalls (fork, exec, map_framebuffer)
     *   2. Physical page alignment (no heap header misalignment)
     *   3. Consistent with the 16 KB per-task kernel stack scheme
     *
     * Was 2 pages (8 KB) — too thin when a timer IRQ preempts a deep
     * syscall call chain on bare metal (PIT @ 1000 Hz), causing RSP to
     * overshoot the stack and corrupt the iretq frame.
     *
     * Note: This stack is used by syscall_entry (via kernel_stack_top)
     * and also set as TSS.RSP0. Per-task kernel stacks in schedule()
     * will override TSS.RSP0 during context switches.
     */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm_off = hhdm_request.response->offset;
    uint64_t phys = (uint64_t)pmm_alloc_contiguous_pages(4);
    if (!phys) {
        serial_write_string("[FATAL] Cannot allocate syscall kernel stack!\n");
        asm volatile ("cli");
        for (;;) asm volatile ("hlt");
    }
    /* Stack grows down: top = base + 16KB */
    kernel_stack_top = phys + hhdm_off + (4 * 4096);
    set_kernel_stack(kernel_stack_top);

    /* Aktifkan syscall MSRs pada BSP. AP mengaktifkannya sendiri di ap_entry(). */
    syscall_init_cpu();

    serial_write_string("[OK] Pintu Gerbang Syscall Berhasil Dibuka!\n");
    serial_write_string("     Syscall: 1=print 2=exit 3=read_key 4=sleep\n");
    serial_write_string("             5=clear 6=print_at 7=draw_char 8=read_file 9=exec 10=fill_rect 11=wait_pid\n");
    serial_write_string("            12=open 13=read 14=close 15=seek 16=readdir\n");
    serial_write_string("            17=shm_create 18=shm_attach 19=shm_detach 20=shm_destroy\n");
    serial_write_string("            21=cache_stats 22=fork\n");
    serial_write_string("            23=write 24=create 25=unlink\n");
    serial_write_string("            26=nice 27=sched_info\n");
    serial_write_string("            28=clone 29=thread_count\n");
    serial_write_string("            37=random 38=sha256 39=aes_enc 40=aes_dec\n");
    serial_write_string("            41=login 42=whoami 43=chmod 44=keystore\n");

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
    in_syscall = 1; /* Debug tracing only — no longer blocks scheduler */
    /*
     * Paksa task ke TASK_RUNNING sebelum memproses syscall.
     * Ini memastikan scheduler memperlakukan task sebagai aktif jika
     * timer interrupt menyala di tengah syscall (preemptible syscalls).
     */
    task_mark_running();
    /* Aktifkan interrupt agar timer dapat preempt syscall yang lama */
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
        fb_fill_rect(0, 0, fb->width, fb->height, 0x002244);
        cursor_force_redraw();
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

    /* ================================================================
     * SYSCALL 26: nice(int nice_value)
     * Adjust scheduling priority via nice value.
     *   nice < 0 = higher priority (more CPU time)
     *   nice > 0 = lower priority (less CPU time)
     *   nice = 0 = default
     * Range: -2 to +2.
     * Return: 0 on success.
     * ================================================================ */
    case 26: {
        int nice_val = (int)(int64_t)arg1;
        result = (uint64_t)(int64_t)task_set_nice(nice_val);
        break;
    }

    /* ================================================================
     * SYSCALL 27: sched_info(uint32_t pid, char* buf)
     * Get scheduling info for a process.
     * pid=0 means current process.
     * Writes info string to buf (must be >= 128 bytes).
     * Return: 0 on success, -1 if not found.
     * ================================================================ */
    case 27: {
        uint32_t target_pid = (uint32_t)arg1;
        char* buf = (char*)arg2;
        result = (uint64_t)(int64_t)task_get_sched_info(target_pid, buf);
        break;
    }

    /* ================================================================
     * SYSCALL 28: clone(uint64_t entry, uint64_t stack_top)
     * Create a new thread in the current process.
     * The thread shares the parent's address space but has its own stack.
     * Return: thread TID (>0) to caller, -1 on error.
     * ================================================================ */
    case 28: {
        uint64_t entry = arg1;
        uint64_t stack = arg2;
        result = (uint64_t)(int64_t)task_clone(entry, stack);
        break;
    }

    /* ================================================================
     * SYSCALL 29: thread_count()
     * Get the number of threads in the current process.
     * Return: thread count (>= 1).
     * ================================================================ */
    case 29: {
        result = (uint64_t)task_get_thread_count();
        break;
    }

    /* ================================================================
     * SYSCALL 30: read_mouse(mouse_state_t* out)
     * Salin snapshot state mouse (x, y, buttons, changed) ke buffer user.
     * Flag 'changed' di-reset ke 0 setelah dibaca (event dikonsumsi).
     * Return: 0 = sukses, -1 = pointer NULL.
     * ================================================================ */
    case 30: {
        mouse_state_t* out = (mouse_state_t*)arg1;
        if (!out) { result = (uint64_t)-1; break; }
        mouse_state_t* s = mouse_get_state();
        out->x       = s->x;
        out->y       = s->y;
        out->buttons = s->buttons;
        out->changed = s->changed;
        s->changed   = 0; /* Konsumsi event */
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 31: get_screen_info(uint32_t* out)
     * Return screen width and height to user-space.
     * out[0] = width, out[1] = height.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 31: {
        uint32_t* out = (uint32_t*)arg1;
        if (!out || !fb) { result = (uint64_t)-1; break; }
        out[0] = (uint32_t)fb->width;
        out[1] = (uint32_t)fb->height;
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 32: draw_pixel((x<<32)|y, color)
     * Draw a single pixel at (x, y) with the given color.
     * Return: 0.
     * ================================================================ */
    case 32: {
        uint32_t x = (uint32_t)(arg1 >> 32);
        uint32_t y = (uint32_t)(arg1 & 0xFFFFFFFF);
        uint32_t color = (uint32_t)arg2;
        fb_draw_pixel(x, y, color);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 33: get_ticks(void)
     * Return current timer tick count (for clock/timing).
     * ================================================================ */
    case 33: {
        result = timer_get_ticks();
        break;
    }

    /* ================================================================
     * SYSCALL 34: map_framebuffer(void)
     * Allocate a back-buffer the same size as the screen and map it
     * into the calling task's address space at 0x70000000.
     * User-space can then write pixels directly to this buffer
     * without any syscalls, achieving near-native rendering speed.
     * Return: virtual address of buffer, or 0 on failure.
     * ================================================================ */
    case 34: {
        if (!fb) { result = 0; break; }

        uint64_t* pml4 = task_get_current_pml4();
        if (!pml4) { result = 0; break; }

        uint64_t base_va = 0x70000000ULL;
        uint32_t w = (uint32_t)fb->width;
        uint32_t h = (uint32_t)fb->height;
        uint64_t buf_size = (uint64_t)w * h * 4;
        uint32_t pages_needed = (uint32_t)((buf_size + 4095) / 4096);

        serial_write_string("[MAPFB] Resolution: ");
        char num[16];
        itoa(w, num, 10); serial_write_string(num);
        serial_write_string("x");
        itoa(h, num, 10); serial_write_string(num);
        serial_write_string(" Pages: ");
        itoa(pages_needed, num, 10); serial_write_string(num);
        serial_write_string("\n");

        if (pages_needed > FB_MAX_PAGES) {
            serial_write_string("[MAPFB] FATAL: Too large!\n");
            result = 0;
            break;
        }

        /*
         * Disable interrupts for the ENTIRE allocation.
         * With O(1) PMM allocator, even 2000+ pages takes ~2ms.
         * The batched cli/sti approach was causing race conditions
         * where the scheduler could preempt between batches and
         * corrupt state, causing crash on first boot.
         */
        asm volatile ("cli");

        /* If pages already allocated (re-entry), just re-map */
        if (fb_phys_count > 0 && fb_phys_count == pages_needed) {
            for (uint32_t i = 0; i < fb_phys_count; i++)
                vmm_map_page_in(pml4, base_va + (uint64_t)i * 4096, fb_phys_pages[i], 0x07);
            fb_mapped_va = base_va;
            asm volatile ("sti");
            result = base_va;
            break;
        }

        /* Allocate pages and map them */
        uint64_t hhdm_off = hhdm_request.response->offset;
        int alloc_ok = 1;

        for (uint32_t i = 0; i < pages_needed; i++) {
            uint64_t phys = (uint64_t)pmm_alloc_page();
            if (!phys) {
                serial_write_string("[MAPFB] OOM!\n");
                alloc_ok = 0;
                break;
            }
            /* Zero the page via HHDM (use 64-bit writes for speed) */
            uint64_t* page_ptr = (uint64_t*)(phys + hhdm_off);
            for (int z = 0; z < 512; z++) page_ptr[z] = 0;

            vmm_map_page_in(pml4, base_va + (uint64_t)i * 4096, phys, 0x07);
            fb_phys_pages[i] = phys;
            fb_phys_count = i + 1;
        }

        asm volatile ("sti");

        if (!alloc_ok) {
            result = 0;
            break;
        }

        fb_mapped_va = base_va;
        serial_write_string("[MAPFB] OK\n");
        result = base_va;
        break;
    }

    /* ================================================================
     * SYSCALL 35: flush_screen(void)
     * Copy the user's back-buffer (at 0x70000000) to the real
     * framebuffer. Since the user's PML4 is active during syscall,
     * we can read directly from the user virtual address.
     * This is the ONLY syscall needed per frame — all pixel writes
     * happen in user-space without any syscalls.
     * Return: 0 on success.
     * ================================================================ */
    case 35: {
        if (!fb) { result = (uint64_t)-1; break; }

        uint32_t w = (uint32_t)fb->width;
        uint32_t h = (uint32_t)fb->height;
        uint32_t pitch_px = (uint32_t)(fb->pitch / 4);
        uint32_t* src = (uint32_t*)0x70000000ULL;
        uint32_t* dst = (uint32_t*)fb->address;

        /*
         * CRITICAL (Bare Metal): Disable interrupts during the copy.
         *
         * src (0x70000000) is mapped ONLY in the calling task's PML4.
         * If the scheduler preempts us and switches CR3 to another
         * task's PML4 (e.g. shell), 0x70000000 is unmapped there →
         * reading src triggers #PF in kernel mode → triple fault →
         * reboot. This is the root cause of the "desktop restart" bug.
         *
         * The copy of a 1920×1080 framebuffer takes ~2-5ms on modern
         * hardware, well within safe cli window.
         */
        asm volatile ("cli");

        for (uint32_t y = 0; y < h; y++) {
            uint32_t* src_row = src + y * w;
            uint32_t* dst_row = dst + y * pitch_px;
            for (uint32_t x = 0; x < w; x++)
                dst_row[x] = src_row[x];
        }

        cursor_force_redraw();
        asm volatile ("sti");
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 36: mouse_stats(mouse_stats_t* out)
     * Ambil snapshot counter diagnostic mouse IRQ12 (bare-metal debug).
     * Lihat dokumentasi interpretasi di drivers/mouse.h.
     * Return: 0 = sukses, -1 = pointer NULL.
     * ================================================================ */
    case 36: {
        mouse_stats_t* out = (mouse_stats_t*)arg1;
        if (!out) { result = (uint64_t)-1; break; }
        out->irq_bytes  = mouse_get_irq_count();
        out->packets    = mouse_get_pkt_count();
        out->sync_drops = mouse_get_sync_drops();
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 37: crypto_random(void* buf, size_t len)
     * Fill buffer with cryptographically random bytes.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 37: {
        void* buf = (void*)arg1;
        size_t len = (size_t)arg2;
        if (!buf || len == 0 || len > 4096) { result = (uint64_t)-1; break; }
        random_bytes(buf, len);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 38: crypto_sha256(void* data, size_t len, uint8_t* digest)
     * Compute SHA-256 hash of data.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 38: {
        const void* data = (const void*)arg1;
        size_t len = (size_t)arg2;
        uint8_t* digest = (uint8_t*)arg3;
        if (!data || !digest) { result = (uint64_t)-1; break; }
        sha256_hash(data, len, digest);
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 39: crypto_aes_encrypt(crypto_aes_params_t* params)
     * AES-256-CBC encrypt using parameter struct.
     * Return: ciphertext length, or -1 on error.
     * ================================================================ */
    case 39: {
        struct {
            const uint8_t* key;
            const uint8_t* iv;
            const void*    input;
            size_t         in_len;
            void*          output;
            size_t         out_max;
        } *p = (void*)arg1;
        if (!p || !p->key || !p->iv || !p->input || !p->output) {
            result = (uint64_t)-1; break;
        }
        int r = aes256_cbc_encrypt(p->key, p->iv, p->input, p->in_len,
                                   p->output, p->out_max);
        result = (uint64_t)(int64_t)r;
        break;
    }

    /* ================================================================
     * SYSCALL 40: crypto_aes_decrypt(crypto_aes_params_t* params)
     * AES-256-CBC decrypt using parameter struct.
     * Return: plaintext length, or -1 on error.
     * ================================================================ */
    case 40: {
        struct {
            const uint8_t* key;
            const uint8_t* iv;
            const void*    input;
            size_t         in_len;
            void*          output;
            size_t         out_max;
        } *p = (void*)arg1;
        if (!p || !p->key || !p->iv || !p->input || !p->output) {
            result = (uint64_t)-1; break;
        }
        int r = aes256_cbc_decrypt(p->key, p->iv, p->input, p->in_len,
                                   p->output, p->out_max);
        result = (uint64_t)(int64_t)r;
        break;
    }

    /* ================================================================
     * SYSCALL 41: crypto_login(char* username, char* password)
     * Authenticate user; sets task UID on success.
     * Return: UID >= 0 on success, -1 on failure.
     * ================================================================ */
    case 41: {
        const char* username = (const char*)arg1;
        const char* password = (const char*)arg2;
        if (!username || !password) { result = (uint64_t)-1; break; }
        int uid = cred_authenticate(username, password);
        if (uid >= 0) {
            /* Set UID on current task */
            task_set_uid((uint32_t)uid, 0);
        }
        result = (uint64_t)(int64_t)uid;
        break;
    }

    /* ================================================================
     * SYSCALL 42: crypto_whoami(char* buf, size_t len)
     * Get current user info.
     * Writes username to buf, returns UID.
     * ================================================================ */
    case 42: {
        char* buf = (char*)arg1;
        size_t len = (size_t)arg2;
        uint32_t uid = task_get_uid();
        if (buf && len > 0) {
            const char* name = cred_get_username(uid);
            if (!name) name = "unknown";
            size_t i;
            for (i = 0; name[i] && i < len - 1; i++)
                buf[i] = name[i];
            buf[i] = '\0';
        }
        result = (uint64_t)uid;
        break;
    }

    /* ================================================================
     * SYSCALL 43: reserved (chmod placeholder)
     * ================================================================ */
    case 43: {
        result = (uint64_t)-1; /* Not yet implemented */
        break;
    }

    /* ================================================================
     * SYSCALL 44: crypto_keystore(crypto_keystore_params_t* params)
     * Encrypted key-value store operations.
     * params->op: 0=SET, 1=GET, 2=DELETE, 3=COUNT, 4=INIT
     * Return: 0 = success, -1 = error. For COUNT, returns count.
     * ================================================================ */
    case 44: {
        struct {
            int         op;
            const char* name;
            void*       value;
            size_t      val_len;
            int*        out_len;
        } *p = (void*)arg1;
        if (!p) { result = (uint64_t)-1; break; }

        switch (p->op) {
        case 0: /* SET */
            result = (uint64_t)(int64_t)keystore_set(p->name, p->value, p->val_len);
            break;
        case 1: /* GET */
            result = (uint64_t)(int64_t)keystore_get(p->name, p->value, p->val_len, p->out_len);
            break;
        case 2: /* DELETE */
            result = (uint64_t)(int64_t)keystore_delete(p->name);
            break;
        case 3: /* COUNT */
            result = (uint64_t)keystore_count();
            break;
        case 4: /* INIT */
            if (p->name) {
                keystore_init(p->name);
                result = 0;
            } else {
                result = (uint64_t)-1;
            }
            break;
        default:
            result = (uint64_t)-1;
        }
        break;
    }

    default:
        serial_write_string("[WARN] Syscall tidak dikenal\n");
        result = (uint64_t)-1;
        break;
    }

    /*
     * Matikan interrupt sebelum kembali ke syscall_entry assembly.
     * CLI melindungi critical path: pop callee-saved regs → sysretq.
     * Tanpa CLI, timer bisa fire saat RSP sudah di-switch ke user stack
     * tapi CPU masih di Ring 0 → stack corruption.
     * sysretq secara atomik me-restore RFLAGS (termasuk IF=1) dari R11.
     */
    asm volatile ("cli");
    in_syscall = 0;
    return result;
}
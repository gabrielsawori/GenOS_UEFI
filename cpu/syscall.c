#include "syscall.h"
#include "fpu.h"
#include "preempt.h"
#include "percpu.h"
#include "msr.h"
#include "gdt.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
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
#include "../security/cred.h"
#include "uaccess.h"
#include "syscall_user.h"
#include "../drivers/input.h"
#include "../drivers/ps2.h"
#include "../drivers/pci.h"
#include "../include/genos/system.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);
extern struct limine_framebuffer *fb;
extern volatile struct limine_hhdm_request hhdm_request;

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
    fpu_init_cpu();
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
    wrmsr(MSR_FMASK, 0x700); /* IF, DF, TF */
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                 : "a"(0x80000001), "c"(0));
    if (d & (1U << 20)) wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ULL << 11));
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    cr0 |= 1ULL << 16; /* Supervisor write protection on every CPU. */
    cr4 &= ~(1ULL << 16); /* No user FSGSBASE until a TLS/GS ABI exists. */
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (b & (1U << 7)) cr4 |= 1ULL << 20;
    asm volatile("mov %0, %%cr0; mov %1, %%cr4" :: "r"(cr0), "r"(cr4) : "memory");
}

void syscall_init(void) {
    serial_write_string("[INFO] Membangun Pintu Gerbang System Call...\n");

    /* Bootstrap stack; scheduling installs each task's stack in per-CPU
     * GS state and TSS.RSP0 before entering Ring 3. */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm_off = hhdm_request.response->offset;
    uint64_t phys = (uint64_t)pmm_alloc_contiguous_pages(4);
    if (!phys) {
        serial_write_string("[FATAL] Cannot allocate syscall kernel stack!\n");
        asm volatile ("cli");
        for (;;) asm volatile ("hlt");
    }
    /* Stack grows down: top = base + 16KB */
    percpu_set_kernel_stack(0, phys + hhdm_off + (4 * 4096));

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
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, const struct registers* user_frame) {
    task_context_enter();
    preempt_disable();
    if (percpu_current()->current && percpu_current()->current->state == TASK_DEAD) {
        preempt_enable();
        exit_current_task();
    }
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
    case 54:
        result = user_alloc(percpu_current()->current, arg1);
        break;
    case 55:
        result = user_free(percpu_current()->current, arg1);
        break;
    case 56:
        result = user_realloc(percpu_current()->current, arg1, arg2);
        break;
    case 50:
        result = smp_get_current_cpu_id();
        break;
    case 51: {
        struct genos_input_event event;
        if (user_write_prepare((void*)arg1, sizeof(event))) { result = -1; break; }
        result = input_peek(&event);
        if (result) {
            if (copy_to_user((void*)arg1, &event, sizeof(event))) result = -1;
            else input_consume();
        }
        break;
    }
    case 52: {
        struct genos_system_info info = {.version = 1,
            .detected_cpus = smp_get_cpu_count(), .current_cpu = smp_get_current_cpu_id(),
            .pci_devices = pci_count(), .ps2_mouse = ps2_mouse_present(),
            .ticks = timer_get_ticks(), .free_bytes = pmm_get_free_ram(),
            .input_dropped = input_dropped()};
        for (uint32_t i = 0; i < smp_get_cpu_count(); i++) {
            info.online_cpus += !!__atomic_load_n(&percpu_get(i)->scheduler_online, __ATOMIC_ACQUIRE);
            info.switches[i] = percpu_get(i)->switches;
        }
        result = copy_to_user((void*)arg1, &info, sizeof(info));
        break;
    }
    case 53: {
        struct pci_device device;
        result = -1;
        if (arg1 <= UINT32_MAX && !pci_get((uint32_t)arg1, &device))
            result = copy_to_user((void*)arg2, &device, sizeof(device));
        break;
    }

    /* ================================================================
     * SYSCALL 1: print(char* text, uint64_t y_pos)
     * Cetak teks ke framebuffer + serial (kompatibilitas lama)
     * ================================================================ */
    case 1: {
        char pesan[USER_STRING_MAX];
        if (copy_user_string(pesan, (const char*)arg1, sizeof(pesan)) < 0) {
            result = (uint64_t)-1; break;
        }
        uint64_t y_pos = arg2 ? arg2 : 100;
        serial_write_string("[APP] ");
        serial_write_string(pesan);
        serial_write_string("\n");
        fb_print("[APP RING-3] -> ", 50, y_pos, 0xFFFF00, 0x002244, 2);
        fb_print(pesan, 250, y_pos, 0xFFFFFF, 0x002244, 2);
        break;
    }

    /* ================================================================
     * SYSCALL 2: exit(int status)
     * Akhiri task yang sedang berjalan. Tidak pernah kembali.
     * ================================================================ */
    case 2:
        serial_write_string("[EXIT] Task called exit()\n");
        preempt_enable();
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

    /* Blocking sleep: dispatcher waits after releasing the mutation lock. */
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
        result = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 6: print_at(char* text, (x << 32) | y, fg_color)
     * Cetak teks pada posisi (x, y) dengan warna foreground tertentu.
     * Koordinat di-pack: arg2 = (x << 32) | (y & 0xFFFFFFFF)
     * ================================================================ */
    case 6: {
        char text[USER_STRING_MAX];
        if (copy_user_string(text, (const char*)arg1, sizeof(text)) < 0) {
            result = (uint64_t)-1; break;
        }
        fb_print(text, (uint32_t)(arg2 >> 32), (uint32_t)arg2,
                 (uint32_t)arg3, 0x002244, 2);
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
        char filename[100];
        if (copy_user_string(filename, (const char*)arg1, sizeof(filename)) < 0 ||
            !is_user_range(arg2, arg3)) { result = (uint64_t)-1; break; }
        size_t file_size = 0;
        char* file_data = tar_read_file(filename, &file_size);
        if (!file_data) break;
        size_t count = file_size < arg3 ? file_size : arg3;
        result = copy_to_user((void*)arg2, file_data, count) ? (uint64_t)-1 : count;
        break;
    }

    /* Spawn an ELF process; constructor publishes only a complete task. */
    case 9: {
        char filename[100];
        if (copy_user_string(filename, (const char*)arg1, sizeof(filename)) < 0) {
            result = (uint64_t)-1; break;
        }
        size_t ukuran = 0;
        char* elf_data = tar_read_file(filename, &ukuran);
        if (elf_data == NULL) {
            serial_write_string("[EXEC] File not found: ");
            serial_write_string(filename);
            serial_write_string("\n");
            result = (uint64_t)-1;
            break;
        }

        PREEMPT_GUARD;
        struct task* child = create_user_task((const uint8_t*)elf_data, ukuran);
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
        char filename[100];
        if (copy_user_string(filename, (const char*)arg1, sizeof(filename)) < 0) {
            result = (uint64_t)-1; break;
        }
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
        result = (uint64_t)(int64_t)sys_user_read((int)arg1, (void*)arg2, arg3);
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
        result = (uint64_t)(int64_t)sys_user_readdir((int)arg1, (char*)arg2, (size_t*)arg3);
        break;
    }

    /* ================================================================
     * SYSCALL 17: shm_create(uint32_t size)
     * Buat shared memory segment baru.
     * Return: shmid (>=0) atau -1 bila gagal.
     * ================================================================ */
    case 17: {
        if (arg1 > UINT32_MAX) { result = (uint64_t)-1; break; }
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
        cache_stats_t snapshot = cache_get_stats();
        result = (uint64_t)(int64_t)copy_to_user((void*)arg1, &snapshot, sizeof(snapshot));
        break;
    }

    /* ================================================================
     * SYSCALL 22: fork(void)
     * Kloning proses saat ini. Child mendapat salinan penuh address space,
     * register state, dan FD table parent.
     * Return: PID child (>0) ke parent, 0 ke child, -1 bila gagal.
     * ================================================================ */
    case 22: {
        int32_t child_pid = task_fork(user_frame);
        result = (uint64_t)(int64_t)child_pid;
        break;
    }

    /* ================================================================
     * SYSCALL 23: write(int fd, void* buf, size_t count)
     * Write data to a file descriptor (must be writable/tmpfs).
     * Return: bytes written or -1 on error.
     * ================================================================ */
    case 23: {
        result = (uint64_t)(int64_t)sys_user_write((int)arg1, (const void*)arg2, arg3);
        break;
    }

    /* ================================================================
     * SYSCALL 24: create(char* filename)
     * Create a new empty file in tmpfs.
     * Return: 0 on success, -1 on error.
     * ================================================================ */
    case 24: {
        char filename[100];
        if (copy_user_string(filename, (const char*)arg1, sizeof(filename)) < 0) {
            result = (uint64_t)-1; break;
        }
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
        char filename[100];
        if (copy_user_string(filename, (const char*)arg1, sizeof(filename)) < 0) {
            result = (uint64_t)-1; break;
        }
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
        char info[128] = {0};
        int status = task_get_sched_info((uint32_t)arg1, info);
        if (!status) status = copy_to_user((void*)arg2, info, sizeof(info));
        result = (uint64_t)(int64_t)status;
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
     * SYSCALL 30: read_mouse (Removed: mouse driver)
     * ================================================================ */
    case 30: {
        /* Removed: mouse driver */
        result = (uint64_t)-1;
        break;
    }

    /* ================================================================
     * SYSCALL 31: get_screen_info(uint32_t* out)
     * Return screen width and height to user-space.
     * out[0] = width, out[1] = height.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 31: {
        if (!fb) { result = (uint64_t)-1; break; }
        uint32_t dimensions[2] = {(uint32_t)fb->width, (uint32_t)fb->height};
        result = (uint64_t)(int64_t)copy_to_user((void*)arg1, dimensions, sizeof(dimensions));
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
     * SYSCALL 34: map_framebuffer (Removed)
     * ================================================================ */
    case 34: {
        result = (uint64_t)-1;
        break;
    }

    /* ================================================================
     * SYSCALL 35: flush_screen (Removed)
     * ================================================================ */
    case 35: {
        result = (uint64_t)-1;
        break;
    }

    /* ================================================================
     * SYSCALL 36: mouse_stats (Removed: mouse driver)
     * ================================================================ */
    case 36: {
        /* Removed: mouse driver */
        result = (uint64_t)-1;
        break;
    }

    /* ================================================================
     * SYSCALL 37: crypto_random(void* buf, size_t len)
     * Fill buffer with cryptographically random bytes.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 37: {
        result = (uint64_t)(int64_t)sys_user_random((void*)arg1, arg2);
        break;
    }

    /* ================================================================
     * SYSCALL 38: crypto_sha256(void* data, size_t len, uint8_t* digest)
     * Compute SHA-256 hash of data.
     * Return: 0 = success, -1 = error.
     * ================================================================ */
    case 38: {
        result = (uint64_t)(int64_t)sys_user_sha256((const void*)arg1, arg2, (void*)arg3);
        break;
    }

    /* ================================================================
     * SYSCALL 39: crypto_aes_encrypt(crypto_aes_params_t* params)
     * AES-256-CBC encrypt using parameter struct.
     * Return: ciphertext length, or -1 on error.
     * ================================================================ */
    case 39: {
        result = (uint64_t)(int64_t)sys_user_aes((const void*)arg1, 0);
        break;
    }

    /* ================================================================
     * SYSCALL 40: crypto_aes_decrypt(crypto_aes_params_t* params)
     * AES-256-CBC decrypt using parameter struct.
     * Return: plaintext length, or -1 on error.
     * ================================================================ */
    case 40: {
        result = (uint64_t)(int64_t)sys_user_aes((const void*)arg1, 1);
        break;
    }

    /* ================================================================
     * SYSCALL 41: crypto_login(char* username, char* password)
     * Authenticate user; sets task UID on success.
     * Return: UID >= 0 on success, -1 on failure.
     * ================================================================ */
    case 41: {
        char username[USERNAME_MAX], password[USER_STRING_MAX];
        if (copy_user_string(username, (const char*)arg1, sizeof(username)) < 0 ||
            copy_user_string(password, (const char*)arg2, sizeof(password)) < 0) {
            result = (uint64_t)-1;
        } else {
            int uid = cred_authenticate(username, password);
            if (uid >= 0) task_set_uid((uint32_t)uid, 0);
            result = (uint64_t)(int64_t)uid;
        }
        for (size_t i = 0; i < sizeof(password); i++) ((volatile char*)password)[i] = 0;
        break;
    }

    /* ================================================================
     * SYSCALL 42: crypto_whoami(char* buf, size_t len)
     * Get current user info.
     * Writes username to buf, returns UID.
     * ================================================================ */
    case 42: {
        uint32_t uid = task_get_uid();
        result = uid;
        if (arg1 && arg2) {
            const char* name = cred_get_username(uid);
            if (!name) name = "unknown";
            char buffer[USERNAME_MAX] = {0};
            size_t capacity = arg2 < sizeof(buffer) ? arg2 : sizeof(buffer);
            size_t i = 0;
            while (i + 1 < capacity && name[i]) { buffer[i] = name[i]; i++; }
            if (copy_to_user((void*)arg1, buffer, i + 1)) result = (uint64_t)-1;
        }
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
        result = (uint64_t)(int64_t)sys_user_keystore((const void*)arg1);
        break;
    }

    /* ================================================================
     * SYSCALL 45: power(uint32_t action)
     * Power management: shutdown or restart the machine.
     *   action=0 → Shutdown (ACPI S5)
     *   action=1 → Restart (keyboard controller reset)
     * Return: -1 on invalid action (should not return on success).
     * ================================================================ */
    case 45: {
        uint32_t action = (uint32_t)arg1;
        serial_write_string("[POWER] ");

        if (action == 0) {
            /* === SHUTDOWN === */
            serial_write_string("Shutting down...\n");

            /* Disable all interrupts */
            asm volatile ("cli");

            /* Method 1: QEMU/Bochs ACPI shutdown (port 0x604) */
            asm volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));

            /* Method 2: VirtualBox ACPI shutdown (port 0x4004) */
            asm volatile ("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x4004));

            /* If still running, halt all CPUs */
            serial_write_string("[POWER] ACPI shutdown failed, halting.\n");
            while (1) asm volatile ("hlt");

        } else if (action == 1) {
            /* === RESTART === */
            serial_write_string("Restarting...\n");

            /* Disable all interrupts */
            asm volatile ("cli");

            /* Method 1: Keyboard controller reset (8042 pulse) */
            /* Wait for input buffer to be empty */
            for (int i = 0; i < 10000; i++) {
                uint8_t status;
                asm volatile ("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x64));
                if (!(status & 0x02)) break;
            }
            /* Send reset command 0xFE to keyboard controller */
            asm volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

            /* Method 2: If 8042 reset didn't work, triple fault */
            /* Load a null IDT and trigger interrupt → guaranteed triple fault */
            struct { uint16_t limit; uint64_t base; } __attribute__((packed))
                null_idt = { 0, 0 };
            asm volatile ("lidt %0" : : "m"(null_idt));
            asm volatile ("int $0x03");

            /* Should never reach here */
            while (1) asm volatile ("hlt");

        } else {
            result = (uint64_t)-1;
        }
        break;
    }

    /* ================================================================
     * SYSCALL 46: set_cursor (Removed: mouse driver)
     * ================================================================ */
    case 46: {
        /* Removed: mouse driver */
        result = (uint64_t)-1;
        break;
    }

    /* ================================================================
     * SYSCALLS 47-49: removed kernel-heap ABI. Never expose kernel pointers.
     * ================================================================ */
    case 47:
    case 48:
    case 49: {
        /*
         * SECURITY: Syscalls kmalloc/kfree/krealloc DIHAPUS.
         * Mengekspos kernel heap ke Ring 3 memungkinkan:
         *   - Arbitrary kernel heap corruption via kfree(kernel_ptr)
         *   - Kernel address leak via kmalloc() return value
         * User allocations use the separate, validated ABI 54-56.
         */
        serial_write_string("[SEC] Blocked: kernel heap syscall from Ring 3\n");
        result = (uint64_t)-1;
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
    preempt_enable();
    task_wait_current();
    return result;
}

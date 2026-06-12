#include "task.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../fs/elf.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include <stddef.h>

static struct task* current_task = NULL;
static struct task* task_list_head = NULL;
static struct task* task_list_tail = NULL;
static uint32_t next_pid = 1;

void task_init(void) {
    serial_write_string("[INFO] Initializing Multitasking Engine...\n");

    struct task* init_task = (struct task*)kmalloc(sizeof(struct task));
    init_task->pid = 0;
    init_task->state = TASK_RUNNING;
    init_task->next = NULL;
    init_task->pml4 = NULL;          /* Kernel task: pakai kernel_pml4 */
    init_task->user_page_count = 0;

    /* Initialize FD table: all slots free */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        init_task->fds[i].type = VFS_TYPE_NONE;
    }

    current_task = init_task;
    task_list_head = init_task;
    task_list_tail = init_task;

    serial_write_string("[OK] Multitasking Engine Active.\n");
}

void create_task(void (*entry_point)(void)) {
    struct task* new_task = (struct task*)kmalloc(sizeof(struct task));
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;

    new_task->stack_base = (uint64_t)kmalloc(4096); 
    uint64_t stack_top = (new_task->stack_base + 4096) & ~0xF;

    for(size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&new_task->regs)[i] = 0;
    }

    new_task->regs.rip = (uint64_t)entry_point; 
    new_task->regs.cs = 0x08;       
    new_task->regs.rflags = 0x202;  
    new_task->regs.rsp = stack_top; 
    new_task->regs.ss = 0x10;       

    new_task->next = NULL;
    task_list_tail->next = new_task;
    task_list_tail = new_task;
}

/*
 * create_user_task() - Meluncurkan aplikasi ELF dalam Ring 3 (User Mode)
 *
 * @param binary_data: Pointer ke data biner ELF yang sudah dibaca dari ramdisk.
 *
 * Fungsi ini:
 *   1. Memuat segmen ELF ke memori user melalui elf_load()
 *   2. Mengalokasikan dan memetakan stack user mode (1 page = 4096 byte)
 *   3. Mendaftarkan task baru ke scheduler dengan selectors Ring-3
 *
 * BUG FIX #6: Stack RSP diset ke (app_stack_virt + 4096 - 16) agar:
 *   - RSP berada DI DALAM page yang di-map (bukan tepat di batasnya)
 *   - RSP 16-byte aligned sesuai System V AMD64 ABI
 *   - Menghindari page fault segera saat fungsi pertama push ke stack
 *
 * BUG FIX #17: create_user_task() sekarang mengembalikan pointer ke task
 *              yang baru dibuat (atau NULL bila gagal). Pointer ini dipakai
 *              syscall `exec` untuk memblokir shell sampai child task
 *              berstatus TASK_DEAD, mencegah balap output yang membuat
 *              prompt baru tertimpa oleh tulisan child.
 */
struct task* create_user_task(uint8_t* binary_data) {
    /* 1. Buat address space baru untuk proses ini */
    uint64_t* new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        serial_write_string("[ERROR] Failed to create address space!\n");
        return NULL;
    }

    /* 2. Daftarkan task ke Scheduler (lebih awal agar pml4 tersedia) */
    struct task* new_task = (struct task*)kmalloc(sizeof(struct task));
    if (!new_task) {
        serial_write_string("[ERROR] Failed to allocate task struct for user task!\n");
        vmm_destroy_address_space(new_pml4);
        return NULL;
    }

    /* Inisialisasi field address space */
    new_task->pml4 = new_pml4;
    new_task->user_page_count = 0;
    for (int i = 0; i < 64; i++) new_task->user_pages[i] = 0;

    /* Initialize FD table: all slots free */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        new_task->fds[i].type = VFS_TYPE_NONE;
    }

    /* 3. Muat ELF ke address space proses baru */
    uint64_t entry_point = elf_load(binary_data, new_pml4,
                                    new_task->user_pages, &new_task->user_page_count);
    if (entry_point == 0) {
        serial_write_string("[ERROR] ELF load gagal: magic number tidak valid!\n");
        vmm_destroy_address_space(new_pml4);
        kfree(new_task);
        return NULL;
    }

    /* 4. Siapkan Stack User Mode (1 page = 4096 byte)
     *
     * Setiap user task mendapat alamat stack virtual UNIK agar tidak saling
     * menimpa. Alamat dimulai dari 0x80000000 dan bertambah 0x10000 (64KB)
     * per task, memberikan ruang guard page antar stack.
     */
    static uint64_t next_user_stack = 0x80000000;
    uint64_t app_stack_virt = next_user_stack;
    next_user_stack += 0x10000; /* 64KB gap antar stack */
    uint64_t phys_stack = (uint64_t)pmm_alloc_page();
    if (!phys_stack) {
        serial_write_string("[ERROR] Failed to allocate user stack!\n");
        vmm_destroy_address_space(new_pml4);
        kfree(new_task);
        return NULL;
    }

    /* Catat stack page untuk cleanup */
    if (new_task->user_page_count < 64) {
        new_task->user_pages[new_task->user_page_count++] = phys_stack;
    }

    /* Petakan stack ke address space proses baru (bukan kernel_pml4!) */
    vmm_map_page_in(new_pml4, app_stack_virt, phys_stack, 0x07); /* User, RW, Present */

    /* 5. Setup register dan state task */
    new_task->pid   = next_pid++;
    new_task->state = TASK_READY;
    new_task->stack_base = app_stack_virt;

    for(size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&new_task->regs)[i] = 0;
    }

    new_task->regs.rip    = entry_point;
    new_task->regs.cs     = 0x23; /* User Code Selector (Ring 3, RPL=3) */
    new_task->regs.rflags = 0x202;
    new_task->regs.rsp = (app_stack_virt + 4096 - 16) & ~0xFULL;
    new_task->regs.ss  = 0x1B; /* User Data Selector (GDT[3], Ring 3, RPL=3) */
    new_task->next = NULL;

    /* Tambahkan task ke antrean scheduler */
    task_list_tail->next = new_task;
    task_list_tail = new_task;
    return new_task;
}

/*
 * schedule() - Pemilih Task Berikutnya (Round-Robin Scheduler)
 *
 * @param current_regs: Pointer ke register CPU saat interrupt timer terjadi.
 *
 * Scheduler menyimpan state program saat ini lalu memilih task berikutnya
 * yang berstatus TASK_READY. Jika semua task DEAD, scheduler berhenti berputar
 * dan mengembalikan ke task saat ini (idle) untuk mencegah infinite loop.
 *
 * BUG FIX #2: Versi lama bisa infinite loop jika SEMUA task berstatus TASK_DEAD
 * karena loop do-while tidak punya kondisi break. Sekarang kita hitung maksimum
 * iterasi = jumlah total task dalam linked list agar aman.
 */
/*
 * Flag dari syscall.c: mencegah scheduler context-switch saat
 * CPU sedang menangani syscall (menggunakan stack terpisah).
 */
extern volatile int in_syscall;

void schedule(struct registers* current_regs) {
    if (!current_task) return;

    /*
     * JANGAN context-switch jika CPU sedang di dalam syscall handler!
     * Syscall menggunakan kernel_stack_top terpisah. Jika scheduler
     * melakukan iretq dari konteks syscall, return path (sysretq)
     * menjadi korup.
     */
    if (in_syscall) return;

    /* Bangunkan task yang sedang tidur jika waktunya sudah tiba */
    uint64_t now = timer_get_ticks();
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->state == TASK_SLEEPING && now >= t->sleep_until) {
            t->state = TASK_READY;
        }
        t = t->next;
    }

    /*
     * === DEAD TASK REAPER ===
     * Bebaskan resource (stack + task struct) dari task yang sudah mati.
     * Dilakukan SETIAP tick agar memori tidak bocor saat proses exit.
     * Hanya reap task yang BUKAN current_task (kita masih pakai stack-nya!).
     */
    struct task* prev = NULL;
    t = task_list_head;
    while (t != NULL) {
        struct task* next = t->next;
        if (t->state == TASK_DEAD && t != current_task) {
            /* Unlink dari linked list */
            if (prev != NULL) {
                prev->next = next;
            } else {
                task_list_head = next;
            }
            if (t == task_list_tail) {
                task_list_tail = prev;
            }
            /* Bangunkan task yang menunggu PID ini (blocking wait_pid) */
            struct task* w = task_list_head;
            while (w != NULL) {
                if (w->state == TASK_WAITING && w->wait_target_pid == t->pid) {
                    w->state = TASK_READY;
                }
                w = w->next;
            }
            /* Bebaskan semua page fisik milik proses ini */
            for (uint32_t pg = 0; pg < t->user_page_count; pg++) {
                if (t->user_pages[pg]) {
                    pmm_free_page((void*)t->user_pages[pg]);
                }
            }
            /* Bebaskan page table levels (lower half) + PML4 */
            if (t->pml4) {
                vmm_destroy_address_space(t->pml4);
            }
            /* Bebaskan task struct */
            kfree(t);
        } else {
            prev = t;
        }
        t = next;
    }

    /* Hitung jumlah total task dalam antrean untuk batas iterasi */
    uint32_t task_count = 0;
    t = task_list_head;
    while (t != NULL) { task_count++; t = t->next; }

    /* Jika hanya ada 1 task (atau tidak ada), tidak perlu switch */
    if (task_count <= 1) return;

    /* Simpan state task saat ini (hanya jika masih hidup dan tidak tidur/menunggu) */
    if (current_task->state != TASK_DEAD &&
        current_task->state != TASK_SLEEPING &&
        current_task->state != TASK_WAITING) {
        current_task->regs = *current_regs;
        if (current_task->state == TASK_RUNNING) current_task->state = TASK_READY;
    }

    /* Cari task berikutnya yang READY (maks iterasi = jumlah task agar tidak loop selamanya) */
    uint32_t attempts = 0;
    do {
        current_task = current_task->next;
        if (current_task == NULL) current_task = task_list_head;
        attempts++;
        /* Hentikan pencarian jika sudah keliling semua task tanpa menemukan READY */
        if (attempts > task_count) {
            /* Semua task DEAD/SLEEPING/WAITING — kembali ke task kepala (init/idle task) */
            current_task = task_list_head;
            break;
        }
    } while (current_task->state == TASK_DEAD ||
             current_task->state == TASK_SLEEPING ||
             current_task->state == TASK_WAITING);

    /* Ganti address space (CR3) sesuai task yang dipilih */
    if (current_task->pml4 != NULL) {
        vmm_switch_pml4(current_task->pml4);
    }
    /* Jika pml4 == NULL (kernel task), CR3 sudah benar dari boot */

    /* Muat register task baru ke CPU */
    current_task->state = TASK_RUNNING;
    *current_regs = current_task->regs;
}

// Fungsi Pengeksekusi Mati
void exit_current_task(void) {
    if (current_task != NULL) {
        current_task->state = TASK_DEAD;

        /* Bangunkan task yang menunggu PID ini (blocking wait_pid).
         * Meskipun reaper juga melakukan ini, kita bangunkan SEKARANG
         * agar parent tidak perlu menunggu tick berikutnya. */
        struct task* w = task_list_head;
        while (w != NULL) {
            if (w->state == TASK_WAITING && w->wait_target_pid == current_task->pid) {
                w->state = TASK_READY;
            }
            w = w->next;
        }
    }
    // Tunggu scheduler untuk membersihkan
    while(1) { asm volatile ("sti; hlt"); }
}

/*
 * sleep_current_task() - Menidurkan task saat ini sampai tick tertentu.
 *
 * @param wake_tick: nilai timer tick di mana task harus dibangunkan.
 *
 * Fungsi ini TIDAK memblokir — hanya menandai task sebagai SLEEPING.
 * Scheduler akan skip task ini sampai timer_get_ticks() >= wake_tick,
 * lalu otomatis membangunkan (ubah state ke TASK_READY).
 */
void sleep_current_task(uint64_t wake_tick) {
    if (current_task != NULL) {
        current_task->sleep_until = wake_tick;
        current_task->state = TASK_SLEEPING;
    }
}

/*
 * task_is_dead() - cek status task berdasarkan PID.
 *
 * Linked list task disusuri untuk menemukan PID yang diminta.
 * Dipakai syscall `wait_pid` agar shell di Ring 3 bisa polling
 * status child task tanpa harus blocking di kernel — yang berbahaya
 * karena `kernel_stack_top` di-share antar syscall.
 *
 * Return: 1 jika task TASK_DEAD atau PID tidak ditemukan, 0 bila masih hidup.
 */
int task_is_dead(uint32_t pid) {
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->pid == pid) {
            return (t->state == TASK_DEAD) ? 1 : 0;
        }
        t = t->next;
    }
    return 1; /* PID tidak ditemukan → anggap selesai */
}

/*
 * task_mark_running() - Tandai task saat ini sebagai TASK_RUNNING.
 *
 * Dipanggil di awal syscall_handler agar task yang sebelumnya tidur
 * (TASK_SLEEPING) tidak dilewati scheduler jika timer interrupt
 * menyala saat task sedang di dalam syscall berikutnya.
 * Tanpa ini, register task bisa disimpan paksa oleh scheduler ke
 * task->regs saat state masih SLEEPING → korupsi state.
 */
void task_mark_running(void) {
    if (current_task != NULL) {
        current_task->state = TASK_RUNNING;
    }
}

/*
 * wait_for_pid() - Blokir task saat ini sampai task target selesai.
 *
 * @param target_pid: PID task yang ditunggu.
 *
 * Jika target sudah TASK_DEAD atau tidak ditemukan, langsung return
 * tanpa memblokir. Jika target masih hidup, set state ke TASK_WAITING
 * dan scheduler akan skip task ini sampai target mati (dibangunkan
 * oleh exit_current_task atau reaper di schedule).
 */
void wait_for_pid(uint32_t target_pid) {
    if (!current_task) return;

    /* Cek apakah target sudah mati atau tidak ada */
    struct task* t = task_list_head;
    int found = 0;
    while (t != NULL) {
        if (t->pid == target_pid) {
            found = 1;
            if (t->state == TASK_DEAD) return; /* Sudah mati, tidak perlu tunggu */
            break;
        }
        t = t->next;
    }

    if (!found) return; /* PID tidak ditemukan, anggap selesai */

    /* Blokir task ini sampai target mati */
    current_task->state = TASK_WAITING;
    current_task->wait_target_pid = target_pid;
}

uint32_t get_current_pid(void) {
    return current_task ? current_task->pid : 0;
}

/*
 * task_get_current_fds() - Return the current task's FD array.
 * Used by VFS layer to access per-process file descriptors.
 */
vfs_fd_t* task_get_current_fds(void) {
    return current_task ? current_task->fds : NULL;
}
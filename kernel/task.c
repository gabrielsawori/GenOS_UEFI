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
    /* 1. Muat ELF ke memori User Mode */
    uint64_t entry_point = elf_load(binary_data);
    if (entry_point == 0) {
        serial_write_string("[ERROR] ELF load gagal: magic number tidak valid!\n");
        return NULL;
    }

    /* 2. Siapkan Stack User Mode (1 page = 4096 byte)
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
        return NULL;
    }
    vmm_map_page(app_stack_virt, phys_stack, 0x07); /* User, RW, Present */

    /* 3. Daftarkan task ke Scheduler */
    struct task* new_task = (struct task*)kmalloc(sizeof(struct task));
    if (!new_task) {
        serial_write_string("[ERROR] Failed to allocate task struct for user task!\n");
        return NULL;
    }
    new_task->pid   = next_pid++;
    new_task->state = TASK_READY;
    new_task->stack_base = app_stack_virt;

    for(size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&new_task->regs)[i] = 0;
    }

    new_task->regs.rip    = entry_point;
    new_task->regs.cs     = 0x23; /* User Code Selector (Ring 3, RPL=3) */
    new_task->regs.rflags = 0x202;
    /*
     * BUG FIX #6: RSP diset ke (base + 4096 - 16) agar:
     * - Berada di DALAM page yang di-map (bukan di tepi atas yang sudah out-of-range)
     * - 16-byte aligned sesuai AMD64 ABI sehingga tidak terjadi #GP saat CALL
     */
    new_task->regs.rsp = (app_stack_virt + 4096 - 16) & ~0xFULL;
    /*
     * BUG FIX: SS = 0x2B menunjuk ke GDT[5] = TSS descriptor → #GP saat iretq!
     * User Data ada di GDT[3] = selector 0x18, dengan RPL=3 menjadi 0x1B.
     */
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

    /* Hitung jumlah total task dalam antrean untuk batas iterasi */
    uint32_t task_count = 0;
    t = task_list_head;
    while (t != NULL) { task_count++; t = t->next; }

    /* Jika hanya ada 1 task (atau tidak ada), tidak perlu switch */
    if (task_count <= 1) return;

    /* Simpan state task saat ini (hanya jika masih hidup dan tidak tidur) */
    if (current_task->state != TASK_DEAD && current_task->state != TASK_SLEEPING) {
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
            /* Semua task DEAD/SLEEPING — kembali ke task kepala (init/idle task) */
            current_task = task_list_head;
            break;
        }
    } while (current_task->state == TASK_DEAD || current_task->state == TASK_SLEEPING);

    /* Muat register task baru ke CPU */
    current_task->state = TASK_RUNNING;
    *current_regs = current_task->regs;
}

// Fungsi Pengeksekusi Mati
void exit_current_task(void) {
    if (current_task != NULL) {
        current_task->state = TASK_DEAD;
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
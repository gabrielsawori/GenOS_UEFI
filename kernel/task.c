#include "task.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/shm.h"
#include "../fs/elf.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../cpu/gdt.h"
#include "../limine.h"
#include "utils.h"
#include <stddef.h>

/* Borrow from pmm.c for HHDM physical page access during fork copy */
extern volatile struct limine_hhdm_request hhdm_request;

/* Boot kernel stack from kernel.c — used as init task's kernel stack */
extern uint8_t boot_kernel_stack[];
#define BOOT_KERNEL_STACK_SIZE 8192

static struct task* current_task = NULL;
static struct task* task_list_head = NULL;
static struct task* task_list_tail = NULL;
static uint32_t next_pid = 1;

/* Quantum lookup table indexed by priority level */
static const uint8_t quantum_table[PRIO_COUNT] = {
    QUANTUM_IDLE,    /* PRIO_IDLE   = 0 */
    QUANTUM_LOW,     /* PRIO_LOW    = 1 */
    QUANTUM_NORMAL,  /* PRIO_NORMAL = 2 */
    QUANTUM_HIGH     /* PRIO_HIGH   = 3 */
};

/* Starvation prevention counter */
static uint64_t last_boost_tick = 0;

/* Helper: clamp priority to valid range */
static inline int8_t clamp_priority(int p) {
    if (p < PRIO_IDLE) return PRIO_IDLE;
    if (p > PRIO_HIGH) return PRIO_HIGH;
    return (int8_t)p;
}

/* Helper: initialize priority fields for a new task */
static void task_init_priority(struct task* t, int8_t prio) {
    t->base_priority = prio;
    t->priority      = prio;
    t->nice          = 0;
    t->quantum_max   = quantum_table[prio];
    t->quantum       = t->quantum_max;
    t->cpu_ticks     = 0;
    t->sleep_count   = 0;
}

void task_init(void) {
    serial_write_string("[INFO] Initializing Multitasking Engine...\n");

    struct task* init_task = (struct task*)kmalloc(sizeof(struct task));
    init_task->pid = 0;
    init_task->state = TASK_RUNNING;
    init_task->next = NULL;
    init_task->pml4 = NULL;          /* Kernel task: pakai kernel_pml4 */
    init_task->user_page_count = 0;

    /*
     * BUG FIX: Init task uses the boot_kernel_stack allocated in kernel.c.
     * This is the same stack set in TSS.RSP0 before interrupts are enabled.
     */
    init_task->kernel_stack = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);

    /* Idle task gets lowest priority — only runs when nothing else can */
    task_init_priority(init_task, PRIO_IDLE);

    /*
     * BUG FIX (CRITICAL): Initialize SS and RSP for init task.
     *
     * In x86-64 long mode, iretq ALWAYS pops SS and RSP from the stack,
     * even when returning to Ring 0 (same privilege). This is DIFFERENT
     * from 32-bit protected mode where SS/RSP are only popped on
     * privilege change.
     *
     * Without valid SS (0x10 = kernel data) and RSP, the first time
     * scheduler switches AWAY from PID 0 and then BACK, iretq loads
     * RSP=0 and SS=0 from task->regs, causing immediate triple fault:
     *   push to RSP=0 => #PF at CR2=0xFFFFFFFFFFFFFFF8
     *   #PF handler tries to use RSP=0 => double fault
     *   double fault handler tries to use RSP=0 => triple fault => REBOOT
     *
     * regs.rsp will be overwritten by schedule() on first save anyway,
     * but it must be non-zero as a safety net.
     */
    for(size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&init_task->regs)[i] = 0;
    }
    init_task->regs.cs     = 0x08; /* Kernel Code */
    init_task->regs.ss     = 0x10; /* Kernel Data */
    init_task->regs.rflags = 0x202;
    init_task->regs.rsp    = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);

    /* Initialize FD table: all slots free */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        init_task->fds[i].type      = VFS_TYPE_NONE;
        init_task->fds[i].writable  = 0;
        init_task->fds[i].tmpfs_idx = -1;
    }

    /* Initialize SHM attachments: all slots free */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        init_task->shm_attached[i].shmid = -1;
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

    /* Kernel tasks default to NORMAL priority */
    task_init_priority(new_task, PRIO_NORMAL);

    /*
     * BUG FIX: Allocate per-task kernel stack (2 pages = 8KB) via PMM.
     * Each task needs its own kernel stack so TSS.RSP0 can be set
     * per-task on context switch.
     */
    uint64_t kstack_phys1 = (uint64_t)pmm_alloc_page();
    uint64_t kstack_phys2 = (uint64_t)pmm_alloc_page();
    uint64_t hhdm_offset = hhdm_request.response->offset;
    if (kstack_phys1 && kstack_phys2) {
        /* Use HHDM mapping to access the physical pages */
        new_task->kernel_stack = kstack_phys2 + hhdm_offset + PAGE_SIZE;
    } else {
        /* Fallback: use parent's stack (not ideal but prevents crash) */
        new_task->kernel_stack = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);
    }

    /* Initialize FD table: all slots free */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        new_task->fds[i].type      = VFS_TYPE_NONE;
        new_task->fds[i].writable  = 0;
        new_task->fds[i].tmpfs_idx = -1;
    }

    /* Initialize SHM attachments: all slots free */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        new_task->shm_attached[i].shmid = -1;
    }

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
    for (int i = 0; i < 256; i++) new_task->user_pages[i] = 0;

    /* Initialize FD table: all slots free */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        new_task->fds[i].type      = VFS_TYPE_NONE;
        new_task->fds[i].writable  = 0;
        new_task->fds[i].tmpfs_idx = -1;
    }

    /* Initialize SHM attachments: all slots free */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        new_task->shm_attached[i].shmid = -1;
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
    if (new_task->user_page_count < 256) {
        new_task->user_pages[new_task->user_page_count++] = phys_stack;
    }

    /* Petakan stack ke address space proses baru (bukan kernel_pml4!) */
    vmm_map_page_in(new_pml4, app_stack_virt, phys_stack, 0x07); /* User, RW, Present */

    /*
     * BUG FIX: Allocate per-task KERNEL stack (2 pages = 8KB) via PMM.
     * When this user task takes an interrupt or syscall, CPU loads
     * TSS.RSP0 as the Ring 0 stack. Each task MUST have its own
     * kernel stack, otherwise concurrent interrupts on different tasks
     * corrupt each other's kernel stack frames.
     *
     * On QEMU this rarely happens because emulation is single-threaded
     * and interrupt timing is predictable. On bare metal, PIT fires
     * at 1000Hz and can interrupt any task at any time.
     */
    uint64_t kstack_phys1 = (uint64_t)pmm_alloc_page();
    uint64_t kstack_phys2 = (uint64_t)pmm_alloc_page();
    uint64_t hhdm_offset = hhdm_request.response->offset;
    if (kstack_phys1 && kstack_phys2) {
        /* Stack grows down, so top = base of second page + PAGE_SIZE */
        new_task->kernel_stack = kstack_phys2 + hhdm_offset + PAGE_SIZE;
    } else {
        serial_write_string("[WARN] Could not allocate per-task kernel stack!\n");
        new_task->kernel_stack = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);
    }

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

    /* User tasks default to NORMAL priority.
     * First user task (shell, PID 1) gets HIGH priority for responsiveness. */
    if (new_task->pid <= 2) {
        task_init_priority(new_task, PRIO_HIGH);
    } else {
        task_init_priority(new_task, PRIO_NORMAL);
    }

    /* Tambahkan task ke antrean scheduler */
    task_list_tail->next = new_task;
    task_list_tail = new_task;
    return new_task;
}

/*
 * schedule() - Priority-Based Round-Robin Scheduler
 *
 * @param current_regs: Pointer ke register CPU saat interrupt timer terjadi.
 *
 * Algoritma:
 *   1. Kurangi quantum task saat ini. Jika masih > 0, TIDAK context-switch
 *      (task masih punya jatah CPU) — kecuali task sudah DEAD/SLEEPING/WAITING.
 *   2. Pilih task READY dengan prioritas tertinggi (priority-based selection).
 *      Jika ada beberapa task dengan prioritas sama, round-robin di antara mereka.
 *   3. I/O-bound boost: task yang sering sleep mendapat +1 prioritas.
 *   4. Starvation prevention: setiap STARVATION_BOOST_INTERVAL ticks,
 *      semua task mendapat priority reset ke base_priority.
 *
 * Quantum per priority:
 *   PRIO_HIGH   = 4 ticks (shell, interactive)
 *   PRIO_NORMAL = 2 ticks (default user procs)
 *   PRIO_LOW    = 1 tick  (background)
 *   PRIO_IDLE   = 1 tick  (kernel idle)
 */
/*
 * kernel_stack_top: digunakan oleh syscall_entry assembly.
 * Di-update per-task oleh scheduler agar syscall selalu menggunakan
 * per-task kernel stack (bukan shared stack). Ini memungkinkan
 * scheduler melakukan preemption di TENGAH syscall — persis seperti Linux.
 */
extern uint64_t kernel_stack_top;

void schedule(struct registers* current_regs) {
    if (!current_task) return;

    /* Bangunkan task yang sedang tidur jika waktunya sudah tiba */
    uint64_t now = timer_get_ticks();
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->state == TASK_SLEEPING && now >= t->sleep_until) {
            t->state = TASK_READY;
            /* I/O-bound boost: task yang baru bangun dari sleep mendapat
             * temporary priority boost agar responsif (seperti CFS) */
            if (t->sleep_count > 5 && t->priority < PRIO_HIGH) {
                t->priority = clamp_priority(t->base_priority + 1);
                t->quantum_max = quantum_table[t->priority];
            }
        }
        t = t->next;
    }

    /*
     * === STARVATION PREVENTION ===
     * Setiap STARVATION_BOOST_INTERVAL ticks, reset priority semua task
     * ke base_priority. Ini mencegah task prioritas rendah kelaparan
     * jika ada task prioritas tinggi yang terus-menerus READY.
     */
    if (now - last_boost_tick >= STARVATION_BOOST_INTERVAL) {
        last_boost_tick = now;
        t = task_list_head;
        while (t != NULL) {
            if (t->state != TASK_DEAD) {
                int eff = t->base_priority - t->nice;
                t->priority = clamp_priority(eff);
                t->quantum_max = quantum_table[t->priority];
                t->quantum = t->quantum_max;
            }
            t = t->next;
        }
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
            /* Cleanup shared memory attachments BEFORE destroying PML4 */
            shm_cleanup_process(t);
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

    /*
     * === QUANTUM CHECK ===
     * Jika task saat ini masih RUNNING dan punya quantum > 0,
     * JANGAN context-switch. Task masih punya jatah CPU.
     * Ini membuat task prioritas tinggi berjalan lebih lama tanpa
     * diganggu, meningkatkan throughput dan mengurangi context-switch overhead.
     */
    if (current_task->state == TASK_RUNNING && current_task->quantum > 0) {
        current_task->quantum--;
        current_task->cpu_ticks++;
        return; /* Tetap di task ini, tidak switch */
    }

    /* Simpan state task saat ini */
    if (current_task->state != TASK_DEAD) {
        current_task->regs = *current_regs;
        if (current_task->state == TASK_RUNNING) {
            current_task->state = TASK_READY;
        }
        current_task->cpu_ticks++;
    }

    /*
     * === PRIORITY-BASED TASK SELECTION ===
     * Cari task READY dengan prioritas tertinggi.
     * Jika ada beberapa task dengan prioritas sama, pilih yang pertama
     * ditemukan setelah current_task (round-robin within same priority).
     */
    struct task* best = NULL;
    int8_t best_prio = -1;

    /* Pass 1: Cari prioritas tertinggi di antara task READY */
    t = task_list_head;
    while (t != NULL) {
        if (t->state == TASK_READY && t->priority > best_prio) {
            best_prio = t->priority;
        }
        t = t->next;
    }

    if (best_prio < 0) {
        /* Tidak ada task READY — kembali ke idle task (head) */
        current_task = task_list_head;
    } else {
        /* Pass 2: Round-robin di antara task READY dengan prioritas tertinggi.
         * Mulai dari task setelah current_task untuk fairness. */
        struct task* start = current_task->next;
        if (start == NULL) start = task_list_head;

        t = start;
        uint32_t attempts = 0;
        while (attempts < task_count) {
            if (t->state == TASK_READY && t->priority == best_prio) {
                best = t;
                break;
            }
            t = t->next;
            if (t == NULL) t = task_list_head;
            attempts++;
        }

        if (best) {
            current_task = best;
        } else {
            current_task = task_list_head;
        }
    }

    /* Reset quantum untuk task yang baru terpilih */
    current_task->quantum = current_task->quantum_max;

    /* Ganti address space (CR3) sesuai task yang dipilih */
    if (current_task->pml4 != NULL) {
        vmm_switch_pml4(current_task->pml4);
    }

    /* Update TSS.RSP0 AND kernel_stack_top for the CURRENT task.
     * TSS.RSP0 = used by CPU for Ring 3→0 interrupt transitions.
     * kernel_stack_top = used by syscall_entry assembly (mov rsp, kernel_stack_top).
     * Both must point to the SAME per-task kernel stack.
     * This enables preemptible syscalls: timer can safely preempt during
     * any syscall because the syscall frame is on the per-task stack,
     * not a shared global stack. */
    if (current_task->kernel_stack) {
        set_kernel_stack(current_task->kernel_stack);
        kernel_stack_top = current_task->kernel_stack;
    }

    /* Muat register task baru ke CPU */
    current_task->state = TASK_RUNNING;
    *current_regs = current_task->regs;

    /* === SERIAL TRACE: Log context switches (first 5 + every 1000th) === */
    {
        static uint32_t switch_count = 0;
        switch_count++;
        if (switch_count <= 5 || (switch_count % 1000) == 0) {
            serial_write_string("[SWITCH #");
            char buf[16];
            itoa(switch_count, buf, 10);
            serial_write_string(buf);
            serial_write_string("] PID ");
            itoa(current_task->pid, buf, 10);
            serial_write_string(buf);
            serial_write_string(" PRI=");
            itoa(current_task->priority, buf, 10);
            serial_write_string(buf);
            serial_write_string(" Q=");
            itoa(current_task->quantum_max, buf, 10);
            serial_write_string(buf);
            serial_write_string(" CS=0x");
            itoa(current_task->regs.cs, buf, 16);
            serial_write_string(buf);
            serial_write_string(" SS=0x");
            itoa(current_task->regs.ss, buf, 16);
            serial_write_string(buf);
            serial_write_string("\n");
        }
    }
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
        current_task->sleep_count++; /* Track I/O-bound behavior */
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

/*
 * task_find_by_pid() - Look up a task by PID in the linked list.
 */
struct task* task_find_by_pid(uint32_t pid) {
    struct task* t = task_list_head;
    while (t != NULL) {
        if (t->pid == pid) return t;
        t = t->next;
    }
    return NULL;
}

/*
 * task_get_current_pml4() - Return the current task's PML4 pointer.
 */
uint64_t* task_get_current_pml4(void) {
    return current_task ? current_task->pml4 : NULL;
}

/*
 * task_fork() - Clone the current process using Copy-on-Write (CoW).
 *
 * Instead of copying every page physically, we:
 *   1. Map the SAME physical pages in both parent and child
 *   2. Mark them READ-ONLY + CoW bit in both address spaces
 *   3. Increment page reference count (pmm_ref_page)
 *   4. On write, page fault handler copies the page on demand
 *
 * This makes fork() O(page_table_entries) instead of O(total_memory).
 *
 * Returns child PID (>0) to parent, or -1 on failure.
 */

/* PTE flag bits for CoW */
#define PTE_WRITABLE 0x002
#define PTE_COW      0x200  /* Bit 9 (Available): marks CoW page */

int32_t task_fork(void) {
    if (!current_task) return -1;
    if (!current_task->pml4) {
        serial_write_string("[FORK] Cannot fork kernel task!\n");
        return -1;
    }

    uint64_t hhdm_offset = hhdm_request.response->offset;
    uint64_t* parent_pml4 = current_task->pml4;

    /* 1. Create child's address space (PML4 with shared upper half) */
    uint64_t* child_pml4 = vmm_create_address_space();
    if (!child_pml4) {
        serial_write_string("[FORK] Failed to create child address space!\n");
        return -1;
    }

    /* 2. Walk parent's lower half, SHARE pages as CoW (read-only) */
    uint64_t child_pages[256];
    uint32_t child_page_count = 0;

    for (int i = 0; i < 256; i++) {
        if (!(parent_pml4[i] & 1)) continue;
        uint64_t* pdpt = (uint64_t*)((parent_pml4[i] & ~0xFFF) + hhdm_offset);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & 1)) continue;
            uint64_t* pd = (uint64_t*)((pdpt[j] & ~0xFFF) + hhdm_offset);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & 1)) continue;
                uint64_t* pt = (uint64_t*)((pd[k] & ~0xFFF) + hhdm_offset);

                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & 1)) continue;

                    /* Found a mapped page - reconstruct VA */
                    uint64_t va = ((uint64_t)i << 39) |
                                  ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) |
                                  ((uint64_t)l << 12);
                    /* Sign-extend if bit 47 is set (canonical upper half) */
                    if (va & (1ULL << 47)) {
                        va |= 0xFFFF000000000000ULL;
                    }

                    /* Only copy user (lower half) pages */
                    if (va >= 0x800000000000ULL && !(va & (1ULL << 47))) continue;
                    if ((va >> 48) != 0 && (va >> 48) != 0xFFFF) continue;

                    uint64_t parent_phys = pt[l] & ~0xFFF;
                    uint64_t flags = pt[l] & 0xFFF;

                    /*
                     * CoW: Mark parent page READ-ONLY + CoW bit.
                     * Clear WRITABLE (bit 1), set CoW (bit 9).
                     */
                    uint64_t cow_flags = (flags & ~PTE_WRITABLE) | PTE_COW;
                    pt[l] = parent_phys | cow_flags;

                    /* Map SAME physical page in child with CoW flags */
                    vmm_map_page_in(child_pml4, va, parent_phys, cow_flags);

                    /* Increment reference count (page now shared) */
                    pmm_ref_page((void*)parent_phys);

                    /* Track page for child cleanup */
                    if (child_page_count < 256) {
                        child_pages[child_page_count++] = parent_phys;
                    }
                }
            }
        }
    }

    /* Flush TLB: parent PTEs changed to read-only */
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    /* 3. Allocate child task struct */
    struct task* child = (struct task*)kmalloc(sizeof(struct task));
    if (!child) {
        serial_write_string("[FORK] Failed to allocate child task struct!\n");
        goto fail_cleanup;
    }

    /* 4. Copy parent's registers, override RAX=0 for child's fork() return
     * Ensure child always gets valid Ring 3 selectors even if parent's
     * saved state was captured during a Ring 0 interrupt (SS/RSP may be stale).
     */
    child->regs = current_task->regs;
    child->regs.rax = 0;  /* fork() returns 0 to child */
    child->regs.cs  = 0x23; /* User Code Selector  — force valid Ring 3 */
    child->regs.ss  = 0x1B; /* User Data Selector  — force valid Ring 3 */

    /* 5. Set up child task fields */
    child->pid = next_pid++;
    child->state = TASK_READY;
    child->pml4 = child_pml4;
    child->stack_base = current_task->stack_base;
    child->sleep_until = 0;
    child->wait_target_pid = 0;

    /* Allocate per-task kernel stack for child */
    uint64_t fork_kstack_phys1 = (uint64_t)pmm_alloc_page();
    uint64_t fork_kstack_phys2 = (uint64_t)pmm_alloc_page();
    if (fork_kstack_phys1 && fork_kstack_phys2) {
        child->kernel_stack = fork_kstack_phys2 + hhdm_offset + PAGE_SIZE;
    } else {
        child->kernel_stack = (uint64_t)(boot_kernel_stack + BOOT_KERNEL_STACK_SIZE);
    }

    /* Copy page tracking */
    child->user_page_count = child_page_count;
    for (uint32_t pg = 0; pg < child_page_count; pg++) {
        child->user_pages[pg] = child_pages[pg];
    }
    for (uint32_t pg = child_page_count; pg < 256; pg++) {
        child->user_pages[pg] = 0;
    }

    /* 6. Copy FD table from parent */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        child->fds[i] = current_task->fds[i];
    }

    /* 7. Clear SHM attachments (child starts fresh) */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        child->shm_attached[i].shmid = -1;
        child->shm_attached[i].vaddr = 0;
        child->shm_attached[i].page_count = 0;
    }

    /* 8. Inherit parent's scheduling priority */
    child->base_priority = current_task->base_priority;
    child->priority      = current_task->priority;
    child->nice          = current_task->nice;
    child->quantum_max   = current_task->quantum_max;
    child->quantum       = child->quantum_max;
    child->cpu_ticks     = 0;
    child->sleep_count   = 0;

    /* 9. Append child to task list */
    child->next = NULL;
    task_list_tail->next = child;
    task_list_tail = child;

    serial_write_string("[FORK-CoW] Child PID ");
    char pid_buf[16];
    itoa(child->pid, pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string(" shares ");
    itoa(child_page_count, pid_buf, 10);
    serial_write_string(pid_buf);
    serial_write_string(" pages (0 copies)\n");

    return (int32_t)child->pid;

fail_cleanup:
    /* Free any allocated child pages */
    for (uint32_t pg = 0; pg < child_page_count; pg++) {
        if (child_pages[pg]) {
            pmm_free_page((void*)child_pages[pg]);
        }
    }
    /* Destroy child's PML4 */
    vmm_destroy_address_space(child_pml4);
    return -1;
}

/*
 * task_set_nice() - Atur nice value untuk task saat ini.
 *
 * Nice value mempengaruhi prioritas efektif:
 *   effective_priority = base_priority - nice
 *
 * Nice negatif (-2, -1) = prioritas LEBIH TINGGI (lebih banyak CPU)
 * Nice positif (+1, +2) = prioritas LEBIH RENDAH (lebih sedikit CPU)
 * Nice 0               = default (tidak ada perubahan)
 *
 * Contoh: task PRIO_NORMAL (2) dengan nice=+1 → efektif PRIO_LOW (1)
 */
int task_set_nice(int nice_val) {
    if (!current_task) return -1;

    /* Clamp nice value to valid range */
    if (nice_val < -2) nice_val = -2;
    if (nice_val >  2) nice_val =  2;

    current_task->nice = (int8_t)nice_val;

    /* Recalculate effective priority */
    int eff = current_task->base_priority - nice_val;
    current_task->priority = clamp_priority(eff);
    current_task->quantum_max = quantum_table[current_task->priority];

    return 0;
}

/*
 * task_get_sched_info() - Dapatkan informasi scheduling untuk task tertentu.
 *
 * Format output: "PID=N PRI=N NICE=N Q=N/M CPU=N SLP=N ST=X\n"
 * Di mana ST = R(unning) | r(eady) | S(leeping) | W(aiting) | D(ead)
 */
int task_get_sched_info(uint32_t pid, char* out_buf) {
    struct task* t;
    if (pid == 0) {
        t = current_task;
    } else {
        t = task_find_by_pid(pid);
    }
    if (!t || !out_buf) return -1;

    char tmp[16];
    char* p = out_buf;

    /* Helper: append string */
    #define APPEND(s) do { const char* _s = (s); while (*_s) *p++ = *_s++; } while(0)

    APPEND("PID=");
    itoa(t->pid, tmp, 10); APPEND(tmp);
    APPEND(" PRI=");
    itoa(t->priority, tmp, 10); APPEND(tmp);
    APPEND(" NICE=");
    if (t->nice < 0) { *p++ = '-'; itoa(-t->nice, tmp, 10); }
    else { itoa(t->nice, tmp, 10); }
    APPEND(tmp);
    APPEND(" Q=");
    itoa(t->quantum, tmp, 10); APPEND(tmp);
    *p++ = '/';
    itoa(t->quantum_max, tmp, 10); APPEND(tmp);
    APPEND(" CPU=");
    itoa(t->cpu_ticks, tmp, 10); APPEND(tmp);
    APPEND(" SLP=");
    itoa(t->sleep_count, tmp, 10); APPEND(tmp);
    APPEND(" ST=");

    switch (t->state) {
        case TASK_RUNNING:  *p++ = 'R'; break;
        case TASK_READY:    *p++ = 'r'; break;
        case TASK_SLEEPING: *p++ = 'S'; break;
        case TASK_WAITING:  *p++ = 'W'; break;
        case TASK_DEAD:     *p++ = 'D'; break;
    }
    *p++ = '\n';
    *p = '\0';

    #undef APPEND
    return 0;
}
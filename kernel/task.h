#pragma once
#include <stdint.h>
#include <stddef.h>     
#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../mm/shm.h" 

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING,  // Task sedang tidur, menunggu timer
    TASK_WAITING,   // Task menunggu task lain selesai (blocking wait_pid)
    TASK_DEAD       // Program telah selesai/mati
} task_state_t;

/*
 * Priority Levels — Menentukan urutan penjadwalan dan quantum.
 * Task dengan prioritas lebih tinggi mendapat giliran CPU lebih sering
 * dan quantum (jatah tick) yang lebih besar.
 *
 * PRIO_HIGH:   Untuk proses interaktif (shell, UI) — quantum 4 ticks
 * PRIO_NORMAL: Default untuk semua proses baru    — quantum 2 ticks
 * PRIO_LOW:    Background tasks                   — quantum 1 tick
 * PRIO_IDLE:   Hanya jalan jika tidak ada yang lain — quantum 1 tick
 */
#define PRIO_IDLE    0
#define PRIO_LOW     1
#define PRIO_NORMAL  2
#define PRIO_HIGH    3
#define PRIO_COUNT   4

/* Quantum (jumlah ticks per giliran) untuk setiap level prioritas */
#define QUANTUM_IDLE    1
#define QUANTUM_LOW     1
#define QUANTUM_NORMAL  2
#define QUANTUM_HIGH    4

/* Starvation prevention: boost semua task setiap N ticks */
#define STARVATION_BOOST_INTERVAL 500

/*
 * User-space heap region (brk-style).
 * 0x60000000–0x67FFFFFF (128MB) — tidak konflik dengan app.elf (0x40000000),
 * shell.elf (0x50000000), maupun stack (0x80000000+).
 */
#define HEAP_BASE       0x60000000ULL
#define HEAP_SIZE_MAX   0x08000000ULL   /* 128MB */

struct task {
    uint32_t pid;
    uint32_t tgid;            // Thread Group ID (= PID of group leader)
    task_state_t state;
    struct registers regs;
    uint64_t stack_base;
    uint64_t sleep_until;     // Tick target untuk bangun dari TASK_SLEEPING
    uint32_t wait_target_pid; // PID yang ditunggu (untuk TASK_WAITING)
    uint64_t* pml4;           // Address space PML4 (NULL = kernel task)
    uint64_t kernel_stack;    // Per-task kernel stack top (for TSS.RSP0)

    /* === User-Space Heap (brk-style, per-process) ===
     * heap_brk:   break saat ini (tumbuh ke atas dari HEAP_BASE).
     *             sbrk(incr) menambah break & memetakan page baru bila perlu.
     * heap_limit: batas atas break (= HEAP_BASE + HEAP_SIZE_MAX). */
    uint64_t heap_brk;
    uint64_t heap_limit;

    /* === Thread Support === */
    uint8_t  is_thread;       // 1 = thread (shares parent's PML4), 0 = process
    uint32_t thread_count;    // Number of threads in this process (leader only)

    /* === Scheduler Priority Fields === */
    int8_t   priority;        // Current priority level (PRIO_IDLE..PRIO_HIGH)
    int8_t   base_priority;   // Original priority (before boost/decay)
    int8_t   nice;            // Nice value (-2..+2, higher = lower priority)
    uint8_t  quantum;         // Current quantum (ticks remaining in this slice)
    uint8_t  quantum_max;     // Max quantum for this priority level
    uint32_t cpu_ticks;       // Total CPU ticks consumed (for stats)
    uint32_t sleep_count;     // Jumlah kali task sleep (I/O indicator)

    uint64_t user_pages[256]; // Daftar page fisik milik proses ini (was 64, now 256 = 1MB max)
    uint32_t user_page_count; // Jumlah page yang dilacak
    vfs_fd_t fds[VFS_MAX_FDS]; // Per-process file descriptor table
    shm_attach_record_t shm_attached[SHM_MAX_ATTACH]; // SHM attachments
    struct task* next;        
};

void task_init(void);
void create_task(void (*entry_point)(void));
void schedule(struct registers* current_regs);
/*
 * create_user_task() - Muat ELF & jadwalkan eksekusi di Ring 3.
 * Mengembalikan pointer ke struct task yang baru dibuat, atau NULL bila
 * gagal. Pointer ini dapat dipakai pemanggil (mis. syscall `exec`) untuk
 * menunggu child task selesai dengan memantau `task->state == TASK_DEAD`.
 */
struct task* create_user_task(uint8_t* binary_data);

// Fungsi untuk membunuh program yang sedang berjalan
void exit_current_task(void);

// Fungsi untuk menidurkan task saat ini sampai tick tertentu
void sleep_current_task(uint64_t wake_tick);

/*
 * task_is_dead() - cek status task berdasarkan PID.
 * Return: 1 jika task TASK_DEAD atau PID tidak ditemukan, 0 bila masih hidup.
 * Dipakai oleh syscall `wait_pid` untuk polling non-blocking dari Ring 3.
 */
int task_is_dead(uint32_t pid);

/*
 * task_mark_running() - Paksa state task saat ini ke TASK_RUNNING.
 * Dipanggil di awal syscall_handler agar task yang sebelumnya tidur
 * tidak dilewati scheduler saat timer menyala di dalam syscall.
 */
void task_mark_running(void);

/*
 * wait_for_pid() - Blokir task saat ini sampai task target selesai.
 * Set state ke TASK_WAITING dan simpan PID target.
 * Scheduler akan membangunkan task ini saat target berstatus TASK_DEAD.
 */
void wait_for_pid(uint32_t target_pid);

/*
 * get_current_pid() - Dapatkan PID task yang sedang berjalan.
 * Dipakai oleh ISR (page fault, GPF) untuk identifikasi proses bermasalah.
 */
uint32_t get_current_pid(void);

/*
 * task_get_current_fds() - Dapatkan array FD milik task saat ini.
 * Dipakai oleh VFS layer untuk mengakses per-process file descriptor table.
 * Return: pointer ke fds[], atau NULL jika tidak ada task aktif.
 */
vfs_fd_t* task_get_current_fds(void);

/*
 * task_find_by_pid() - Look up a task by PID in the linked list.
 * Return: pointer to task, or NULL if not found.
 */
struct task* task_find_by_pid(uint32_t pid);

/*
 * task_get_current_pml4() - Get the current task's PML4 pointer.
 * Return: PML4 pointer, or NULL for the kernel task.
 */
uint64_t* task_get_current_pml4(void);

/*
 * task_fork() - Clone the current process.
 * Creates a child with copied address space, registers, and FD table.
 * Returns child PID (>0) to parent, 0 is set for child (via regs.rax),
 * or -1 on failure.
 */
int32_t task_fork(void);

/*
 * task_set_nice() - Set nice value for the current task.
 * @param nice: Nice value (-2 to +2). Negative = higher priority.
 * Adjusts effective priority = base_priority - nice (clamped).
 * Return: 0 on success.
 */
int task_set_nice(int nice);

/*
 * task_get_sched_info() - Get scheduling info for a task.
 * @param pid: Target PID (0 = current task)
 * @param out_buf: Output buffer (at least 128 bytes)
 * Writes: "PID=N PRI=N NICE=N QTICK=N/M CPU=N SLP=N STATE=X"
 * Return: 0 on success, -1 if not found.
 */
int task_get_sched_info(uint32_t pid, char* out_buf);

/*
 * task_clone() - Create a new thread in the current process.
 * @param entry: Thread entry point function address (user-space)
 * @param stack_top: Top of the thread's stack (user-allocated)
 *
 * The new thread shares the parent's address space (PML4), FDs, and SHM.
 * It gets its own: PID/TID, registers, kernel stack, scheduling state.
 *
 * Returns thread TID (>0) to parent, or -1 on failure.
 * Thread starts executing at entry with RSP=stack_top.
 */
int32_t task_clone(uint64_t entry, uint64_t stack_top);

/*
 * task_get_thread_count() - Get number of threads in current process.
 * Returns thread_count for the current task's thread group leader.
 */
uint32_t task_get_thread_count(void);
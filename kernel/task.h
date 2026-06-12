#pragma once
#include <stdint.h>
#include <stddef.h>     
#include "../cpu/isr.h"
#include "../fs/vfs.h" 

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING,  // Task sedang tidur, menunggu timer
    TASK_WAITING,   // Task menunggu task lain selesai (blocking wait_pid)
    TASK_DEAD       // Program telah selesai/mati
} task_state_t;

struct task {
    uint32_t pid;             
    task_state_t state;       
    struct registers regs;    
    uint64_t stack_base;      
    uint64_t sleep_until;     // Tick target untuk bangun dari TASK_SLEEPING
    uint32_t wait_target_pid; // PID yang ditunggu (untuk TASK_WAITING)
    uint64_t* pml4;           // Address space PML4 (NULL = kernel task)
    uint64_t user_pages[64];  // Daftar page fisik milik proses ini
    uint32_t user_page_count; // Jumlah page yang dilacak
    vfs_fd_t fds[VFS_MAX_FDS]; // Per-process file descriptor table
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
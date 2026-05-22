#pragma once
#include <stdint.h>
#include "../cpu/isr.h" // Kita butuh struktur register dari sini

// Status program saat ini
typedef enum {
    TASK_RUNNING,
    TASK_READY
} task_state_t;

// Process Control Block (KTP Program)
struct task {
    uint32_t pid;             // ID Program
    task_state_t state;       // Status (Jalan / Antre)
    struct registers regs;    // Salinan Otak CPU (Register)
    uint64_t stack_base;      // Ruang memori khusus (Stack) untuk program ini
    struct task* next;        // Antrean ke program selanjutnya
};

void task_init(void);
void create_task(void (*entry_point)(void));
void delete_task(struct task* task);  // FIX BUG #4: Tambahkan deklarasi delete_task
void schedule(struct registers* current_regs);

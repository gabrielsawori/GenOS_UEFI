#pragma once
#include <stdint.h>
#include <stddef.h>     
#include "../cpu/isr.h" 

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_DEAD   // <--- STATUS BARU: Program telah selesai/mati
} task_state_t;

struct task {
    uint32_t pid;             
    task_state_t state;       
    struct registers regs;    
    uint64_t stack_base;      
    struct task* next;        
};

void task_init(void);
void create_task(void (*entry_point)(void));
void schedule(struct registers* current_regs);
void create_user_task(uint8_t* binary_data);

// Fungsi baru untuk membunuh program yang sedang berjalan
void exit_current_task(void);
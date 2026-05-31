#include "task.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../fs/elf.h"
#include "../drivers/serial.h"
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

// Fungsi Peluncur Aplikasi ELF (Ring 3)
void create_user_task(uint8_t* binary_data) {
    // 1. Muat ELF ke memori User Mode
    uint64_t entry_point = elf_load(binary_data);
    if (entry_point == 0) return;

    // 2. Siapkan Stack User Mode
    uint64_t app_stack_virt = 0x80000000;
    uint64_t phys_stack = (uint64_t)pmm_alloc_page();
    vmm_map_page(app_stack_virt, phys_stack, 7); 

    // 3. Daftarkan KTP ke Scheduler
    struct task* new_task = (struct task*)kmalloc(sizeof(struct task));
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;

    for(size_t i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&new_task->regs)[i] = 0;
    }

    new_task->regs.rip = entry_point; 
    new_task->regs.cs = 0x23; // Segmen Kode User
    new_task->regs.rflags = 0x202;
    new_task->regs.rsp = app_stack_virt + 4096;
    new_task->regs.ss = 0x1B; // Segmen Data User

    new_task->next = NULL;
    task_list_tail->next = new_task;
    task_list_tail = new_task;
}

// Pengatur Antrean (Scheduler)
void schedule(struct registers* current_regs) {
    if (!current_task || (current_task->next == NULL && task_list_head == task_list_tail)) return;

    // Simpan status program saat ini (HANYA JIKA IA MASIH HIDUP)
    if (current_task->state != TASK_DEAD) {
        current_task->regs = *current_regs;
        if (current_task->state == TASK_RUNNING) current_task->state = TASK_READY;
    }

    // Geser ke program selanjutnya di antrean
    do {
        current_task = current_task->next;
        if (current_task == NULL) current_task = task_list_head; 
    } while (current_task->state == TASK_DEAD); // TERUS LOMPATI JIKA PROGRAM SUDAH MATI!

    // Muat register program baru ke dalam CPU
    current_task->state = TASK_RUNNING;
    *current_regs = current_task->regs; 
}

// Fungsi Pengeksekusi Mati
void exit_current_task(void) {
    if (current_task != NULL) {
        current_task->state = TASK_DEAD;
    }
    // Tunggu "Malaikat Maut" (Timer Interrupt) datang untuk membuangnya dari CPU
    while(1) { asm volatile ("sti; hlt"); }
}
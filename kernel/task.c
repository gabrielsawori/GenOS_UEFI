#include "task.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../drivers/serial.h"

static struct task* current_task = NULL;
static struct task* task_list_head = NULL;
static struct task* task_list_tail = NULL;
static uint32_t next_pid = 1;

void task_init(void) {
    serial_write_string("[INFO] Menginisialisasi Multitasking Engine...\n");

    // Buat KTP untuk sistem utama
    struct task* init_task = (struct task*)kmalloc(sizeof(struct task));
    init_task->pid = 0;
    init_task->state = TASK_RUNNING;
    init_task->next = NULL;

    current_task = init_task;
    task_list_head = init_task;
    task_list_tail = init_task;

    serial_write_string("[OK] Multitasking Engine Aktif.\n");
}

void create_task(void (*entry_point)(void)) {
    struct task* new_task = (struct task*)kmalloc(sizeof(struct task));
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;

    // --- KUNCI PERBAIKANNYA DI SINI ---
    // Gunakan kmalloc() agar mendapat alamat Virtual yang sah di mata CPU!
    new_task->stack_base = (uint64_t)kmalloc(4096); 
    
    // Pastikan ujung tumpukan memori (Stack Top) sejajar 16-byte sesuai aturan mesin 64-bit
    uint64_t stack_top = (new_task->stack_base + 4096) & ~0xF;

    // Bersihkan KTP register
    for(int i = 0; i < sizeof(struct registers); i++) {
        ((uint8_t*)&new_task->regs)[i] = 0;
    }

    new_task->regs.rip = (uint64_t)entry_point; 
    new_task->regs.cs = 0x08;       
    new_task->regs.rflags = 0x202;  
    new_task->regs.rsp = stack_top; 
    new_task->regs.ss = 0x10;       

    // Masukkan ke antrean Scheduler
    new_task->next = NULL;
    task_list_tail->next = new_task;
    task_list_tail = new_task;
}

void schedule(struct registers* current_regs) {
    if (!current_task || (current_task->next == NULL && task_list_head == task_list_tail)) return;

    // Bekukan program saat ini
    current_task->regs = *current_regs;
    if (current_task->state == TASK_RUNNING) current_task->state = TASK_READY;

    // Pindah ke program selanjutnya di antrean
    current_task = current_task->next;
    if (current_task == NULL) current_task = task_list_head; 

    // Muat register program baru ke dalam CPU
    current_task->state = TASK_RUNNING;
    *current_regs = current_task->regs; 
}
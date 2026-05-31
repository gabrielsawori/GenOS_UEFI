#include "syscall.h"
#include "msr.h"
#include "gdt.h" 
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../mm/heap.h"
#include "../kernel/task.h" // Tambahkan header task agar tahu fungsi exit_current_task()

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);

uint64_t kernel_stack_top = 0;

void syscall_init(void) {
    serial_write_string("[INFO] Membangun Pintu Gerbang System Call...\n");

    uint64_t stack_base = (uint64_t)kmalloc(4096);
    kernel_stack_top = stack_base + 4096;
    set_kernel_stack(kernel_stack_top);

    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);

    uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x10 << 48);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);

    serial_write_string("[OK] Pintu Gerbang Syscall Berhasil Dibuka!\n");
}

void syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg3; 
    
    if (syscall_num == 1) { // SYSCALL NOMOR 1: PRINT
        char* pesan = (char*)arg1;
        fb_print("[APP RING-3] -> ", 50, arg2, 0xFFFF00, 0x002244, 2);
        fb_print(pesan, 250, arg2, 0xFFFFFF, 0x002244, 2);
    }
    else if (syscall_num == 2) { // SYSCALL NOMOR 2: EXIT
        exit_current_task();
    }
}
#include <stdint.h>
#include <stddef.h>
#include "../limine.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "utils.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/heap.h"
#include "task.h"
#include "../cpu/syscall.h"
#include "../fs/tar.h"

LIMINE_BASE_REVISION(1)

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0 };

__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_request = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };

__attribute__((used, section(".requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0
};

static void hcf(void) { asm ("cli"); for (;;) { asm ("hlt"); } }

static int cursor_x = 50;
static int cursor_y = 100;
struct limine_framebuffer *fb; 

void clear_screen() {
    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) { fb_draw_pixel(x, y, 0x002244); }
    }
    cursor_x = 50;
    cursor_y = 50;
}

void terminal_print(const char* text, uint32_t color) {
    fb_print(text, cursor_x, cursor_y, color, 0x002244, 2);
    cursor_y += 24;
    cursor_x = 50;
}

void shell_task(void) {
    clear_screen();
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("           GENOS SYSTEM TERMINAL V3          ", 0x00FF00);
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("Type 'help' to see available commands.", 0xAAAAAA);
    cursor_y += 10;

    char cmd_buffer[256];
    int cmd_index = 0;

    fb_print("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF, 0x002244, 2);
    cursor_x += 16 * 16; 
    int prompt_batas_kiri = cursor_x; 

    while(1) {
        char c = keyboard_get_char();

        if (c != 0) { 
            if (c == '\n') { 
                cmd_buffer[cmd_index] = '\0'; 
                cursor_y += 24; 
                cursor_x = 50;  
                
                if (cmd_index > 0) {
                    if (strcmp(cmd_buffer, "help") == 0) {
                        terminal_print("Available Commands:", 0xFFFF00);
                        terminal_print("  help : Show this message", 0xFFFFFF);
                        terminal_print("  clear: Clear the terminal screen", 0xFFFFFF);
                        terminal_print("  info : OS Information", 0xFFFFFF);
                        terminal_print("  read : Read pesan.txt from Ramdisk", 0xFFFFFF);
                        terminal_print("  run  : Execute app.elf in Ring 3!", 0x00FF00);
                    } 
                    else if (strcmp(cmd_buffer, "clear") == 0) { 
                        clear_screen(); 
                    }
                    else if (strcmp(cmd_buffer, "info") == 0) {
                        terminal_print("GenOS v3 (64-bit UEFI) - Built by Mandor", 0x00FF00);
                        terminal_print("Architecture: x86_64 | Security: User Mode Ring-3 | Format: ELF", 0x00FF00);
                    }
                    else if (strcmp(cmd_buffer, "read") == 0) {
                        size_t ukuran = 0;
                        char* isi_file = tar_read_file("pesan.txt", &ukuran);
                        
                        if (isi_file != NULL) {
                            char temp[256];
                            size_t copy_size = ukuran < 255 ? ukuran : 255;
                            for(size_t i=0; i<copy_size; i++) temp[i] = isi_file[i];
                            temp[copy_size] = '\0';
                            
                            terminal_print("Extracting pesan.txt from Ramdisk:", 0xFFFF00);
                            terminal_print(temp, 0xFFFFFF);
                        } else {
                            terminal_print("[ERROR] pesan.txt not found!", 0xFF0000);
                        }
                    }
                    else if (strcmp(cmd_buffer, "run") == 0) {
                        size_t ukuran = 0;
                        // MENCARI FILE ELF, BUKAN BIN
                        char* app_data = tar_read_file("app.elf", &ukuran);
                        
                        if (app_data != NULL) {
                            terminal_print("Loading ELF application into User Mode (Ring 3)...", 0x00FF00);
                            // MEMANGGIL DENGAN 1 ARGUMEN SAJA
                            create_user_task((uint8_t*)app_data); 
                        } else {
                            terminal_print("[ERROR] app.elf not found in Ramdisk!", 0xFF0000);
                        }
                    }
                    else {
                        fb_print("Command not found: ", cursor_x, cursor_y, 0xFF0000, 0x002244, 2);
                        fb_print(cmd_buffer, cursor_x + 260, cursor_y, 0xFFFF00, 0x002244, 2);
                        cursor_y += 24;
                    }
                }
                
                cmd_index = 0; 
                fb_print("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF, 0x002244, 2);
                cursor_x += 16 * 16;
                prompt_batas_kiri = cursor_x;
            } 
            else if (c == '\b') { 
                if (cmd_index > 0) { 
                    cmd_index--; 
                    cursor_x -= 16; 
                    fb_draw_char(' ', cursor_x, cursor_y, 0x000000, 0x002244, 2);
                }
            } 
            else { 
                if (cmd_index < 254) { 
                    cmd_buffer[cmd_index++] = c; 
                    fb_draw_char(c, cursor_x, cursor_y, 0xFFFFFF, 0x002244, 2);
                    cursor_x += 16; 
                    if (cursor_x > 750) { cursor_y += 24; cursor_x = prompt_batas_kiri; }
                }
            }
        } 
        else { 
            asm volatile ("hlt"); 
        }
    }
}

void _start(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == 0) hcf();

    serial_init();
    serial_write_string("[INFO] Booting GenOS v3...\n");

    gdt_init();
    idt_init(); 
    pic_remap();
    timer_init(1000); 

    if (framebuffer_request.response == NULL || memmap_request.response == NULL) hcf();
    fb = framebuffer_request.response->framebuffers[0]; 
    fb_init(fb);

    serial_write_string("[INFO] Initializing Memory Management...\n");
    pmm_init();
    vmm_init();
    heap_init(); 
    
    serial_write_string("[INFO] Starting Task Scheduler...\n");
    task_init();

    if (module_request.response != NULL && module_request.response->module_count > 0) {
        tar_init(module_request.response->modules[0]->address);
    }

    create_task(shell_task);
    syscall_init();

    asm volatile ("sti");
    while(1) { asm volatile ("hlt"); }
}
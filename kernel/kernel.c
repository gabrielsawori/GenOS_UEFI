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

LIMINE_BASE_REVISION(1)

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0
};
__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0
};

static void hcf(void) { asm ("cli"); for (;;) { asm ("hlt"); } }

// Variabel Global untuk Kursor Layar
static int cursor_x = 50;
static int cursor_y = 100;
struct limine_framebuffer *fb; // Kita simpan pointer layar agar bisa dipakai fungsi clear

// Fungsi untuk membersihkan seluruh layar dengan warna biru
void clear_screen() {
    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) { fb_draw_pixel(x, y, 0x002244); }
    }
    cursor_x = 50;
    cursor_y = 50;
}

// Fungsi untuk mencetak teks ke terminal dan otomatis turun baris
void terminal_print(const char* text, uint32_t color) {
    fb_print(text, cursor_x, cursor_y, color, 0x002244, 2);
    cursor_y += 24;
    cursor_x = 50;
}

// ==========================================
// PROGRAM TERMINAL (SHELL TASK)
// ==========================================
void shell_task(void) {
    clear_screen();
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("        GENOS COMMAND TERMINAL V3            ", 0x00FF00);
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("Ketik 'help' untuk melihat daftar perintah.", 0xAAAAAA);
    cursor_y += 10;

    // Buffer untuk menyimpan teks yang sedang diketik
    char cmd_buffer[256];
    int cmd_index = 0;

    // Cetak Prompt Pertama
    fb_print("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF, 0x002244, 2);
    cursor_x += 16 * 16; 
    int prompt_batas_kiri = cursor_x; 

    while(1) {
        char c = keyboard_get_char();

        if (c != 0) { 
            if (c == '\n') { // ENTER DITEKAN!
                cmd_buffer[cmd_index] = '\0'; // Tutup string perintah dengan karakter null
                cursor_y += 24; 
                cursor_x = 50;  
                
                // --- EVALUASI PERINTAH ---
                if (cmd_index > 0) {
                    if (strcmp(cmd_buffer, "help") == 0) {
                        terminal_print("Daftar Perintah Tersedia:", 0xFFFF00);
                        terminal_print("  help  : Menampilkan pesan ini", 0xFFFFFF);
                        terminal_print("  clear : Membersihkan layar terminal", 0xFFFFFF);
                        terminal_print("  info  : Menampilkan informasi sistem OS", 0xFFFFFF);
                    } 
                    else if (strcmp(cmd_buffer, "clear") == 0) {
                        clear_screen();
                    }
                    else if (strcmp(cmd_buffer, "info") == 0) {
                        terminal_print("GenOS v3 (64-bit UEFI) - Dibuat oleh Mandor", 0x00FF00);
                        terminal_print("Arsitektur: x86_64 | Memory Manager: PMM & VMM", 0x00FF00);
                    }
                    else {
                        // Perintah tidak dikenal
                        fb_print("Perintah tidak ditemukan: ", cursor_x, cursor_y, 0xFF0000, 0x002244, 2);
                        fb_print(cmd_buffer, cursor_x + 400, cursor_y, 0xFFFF00, 0x002244, 2);
                        cursor_y += 24;
                    }
                }

                // Reset buffer untuk perintah selanjutnya
                cmd_index = 0;

                // Cetak ulang Prompt untuk baris baru
                fb_print("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF, 0x002244, 2);
                cursor_x += 16 * 16;
                prompt_batas_kiri = cursor_x;
            } 
            else if (c == '\b') { // BACKSPACE
                if (cmd_index > 0) { 
                    cmd_index--; // Hapus dari memori buffer
                    cursor_x -= 16; // Mundur 1 langkah di layar
                    fb_draw_char(' ', cursor_x, cursor_y, 0x000000, 0x002244, 2);
                }
            } 
            else { // Ketikan huruf biasa
                if (cmd_index < 254) { // Jangan sampai ember kepenuhan
                    cmd_buffer[cmd_index++] = c; // Simpan ke buffer
                    fb_draw_char(c, cursor_x, cursor_y, 0xFFFFFF, 0x002244, 2);
                    cursor_x += 16; 

                    if (cursor_x > 750) { // Wrap baris
                        cursor_y += 24;
                        cursor_x = prompt_batas_kiri;
                    }
                }
            }
        } 
        else {
            asm volatile ("hlt"); // CPU Istirahat
        }
    }
}

void _start(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == 0) hcf();

    serial_init();
    gdt_init();
    idt_init(); 
    pic_remap();
    timer_init(1000); 

    if (framebuffer_request.response == NULL || memmap_request.response == NULL) hcf();
    fb = framebuffer_request.response->framebuffers[0]; // Simpan ke variabel global
    fb_init(fb);

    pmm_init();
    vmm_init();
    heap_init(); 
    task_init();

    // Daftarkan Program Shell kita
    create_task(shell_task);

    asm volatile ("sti");

    while(1) { asm volatile ("hlt"); }
}
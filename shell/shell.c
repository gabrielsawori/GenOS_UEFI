/*
 * GenOS System Shell (Ring 3 — User Space)
 *
 * Shell ini berjalan sepenuhnya di Ring 3. Semua komunikasi dengan kernel
 * dilakukan melalui system call. Jika shell crash, kernel tetap aman.
 *
 * Syscall yang digunakan:
 *   5 (clear_screen)  - bersihkan layar
 *   6 (print_at)      - cetak teks pada posisi (x,y)
 *   7 (draw_char)     - gambar 1 karakter
 *   3 (read_key)      - baca keyboard
 *   4 (sleep)         - delay
 *   8 (read_file)     - baca file dari ramdisk
 *   9 (exec)          - jalankan program ELF
 *   2 (exit)          - keluar
 */
#include "../libc/stdio.h"
#include "../libc/stdlib.h"
#include "../libc/string.h"

/* Posisi kursor terminal (dikelola di user-space) */
static int cursor_x = 50;
static int cursor_y = 50;

/* Cetak teks lalu pindah kursor ke baris berikutnya */
static void terminal_print(const char* text, uint32_t color) {
    print_at(text, cursor_x, cursor_y, color);
    cursor_y += 24;
    cursor_x = 50;
}

/* Cetak prompt shell */
static void print_prompt(void) {
    print_at("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF);
    cursor_x += 16 * 16; /* Lebar prompt = 16 karakter × 16 piksel */
}

void _start(void) {
    /* --- LAYAR SELAMAT DATANG --- */
    clear_screen();
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("           GENOS SYSTEM TERMINAL V3          ", 0x00FF00);
    terminal_print("        [Ring 3 — Isolated User Space]       ", 0x00CC00);
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("Type 'help' to see available commands.", 0xAAAAAA);
    cursor_y += 10;

    char cmd_buffer[256];
    int cmd_index = 0;

    print_prompt();
    int prompt_batas_kiri = cursor_x;

    /* --- LOOP UTAMA SHELL --- */
    while (1) {
        char c = read_key();

        if (c != 0) {
            /* ===== ENTER: Eksekusi perintah ===== */
            if (c == '\n') {
                cmd_buffer[cmd_index] = '\0';
                cursor_y += 24;
                cursor_x = 50;

                if (cmd_index > 0) {
                    /* --- HELP --- */
                    if (strcmp(cmd_buffer, "help") == 0) {
                        terminal_print("Available Commands:", 0xFFFF00);
                        terminal_print("  help  : Show this message", 0xFFFFFF);
                        terminal_print("  clear : Clear the terminal screen", 0xFFFFFF);
                        terminal_print("  info  : OS Information", 0xFFFFFF);
                        terminal_print("  read  : Read pesan.txt from Ramdisk", 0xFFFFFF);
                        terminal_print("  run   : Execute app.elf in Ring 3!", 0x00FF00);
                    }
                    /* --- CLEAR --- */
                    else if (strcmp(cmd_buffer, "clear") == 0) {
                        clear_screen();
                        cursor_x = 50;
                        cursor_y = 50;
                    }
                    /* --- INFO --- */
                    else if (strcmp(cmd_buffer, "info") == 0) {
                        terminal_print("GenOS v3 (64-bit UEFI) - Built by Mandor", 0x00FF00);
                        terminal_print("Architecture: x86_64 | Security: Ring-3 Shell", 0x00FF00);
                        terminal_print("Shell berjalan di User Space (terisolasi)", 0x00FF00);
                    }
                    /* --- READ --- */
                    else if (strcmp(cmd_buffer, "read") == 0) {
                        char file_buf[256];
                        int bytes = read_file("pesan.txt", file_buf, 255);
                        if (bytes > 0) {
                            file_buf[bytes] = '\0';
                            terminal_print("Extracting pesan.txt from Ramdisk:", 0xFFFF00);
                            terminal_print(file_buf, 0xFFFFFF);
                        } else {
                            terminal_print("[ERROR] pesan.txt not found!", 0xFF0000);
                        }
                    }
                    /* --- RUN --- */
                    else if (strcmp(cmd_buffer, "run") == 0) {
                        terminal_print("Loading app.elf into Ring 3...", 0x00FF00);
                        int result = exec("app.elf");
                        if (result != 0) {
                            terminal_print("[ERROR] app.elf not found!", 0xFF0000);
                        }
                    }
                    /* --- COMMAND NOT FOUND --- */
                    else {
                        print_at("Command not found: ", cursor_x, cursor_y, 0xFF0000);
                        print_at(cmd_buffer, cursor_x + 260, cursor_y, 0xFFFF00);
                        cursor_y += 24;
                    }
                }

                cmd_index = 0;
                print_prompt();
                prompt_batas_kiri = cursor_x;
            }
            /* ===== BACKSPACE: Hapus karakter ===== */
            else if (c == '\b') {
                if (cmd_index > 0) {
                    cmd_index--;
                    cursor_x -= 16;
                    draw_char(' ', cursor_x, cursor_y, 0x002244);
                }
            }
            /* ===== KARAKTER BIASA: Tampilkan dan simpan ===== */
            else {
                if (cmd_index < 254) {
                    cmd_buffer[cmd_index++] = c;
                    draw_char(c, cursor_x, cursor_y, 0xFFFFFF);
                    cursor_x += 16;
                    if (cursor_x > 750) {
                        cursor_y += 24;
                        cursor_x = prompt_batas_kiri;
                    }
                }
            }
        }
        else {
            /* Tidak ada input — tidur 10ms agar hemat CPU */
            user_sleep(10);
        }
    }
}

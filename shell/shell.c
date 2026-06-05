/*
 * GenOS System Shell (Ring 3 — User Space)
 *
 * Shell ini berjalan sepenuhnya di Ring 3. Semua komunikasi dengan kernel
 * dilakukan melalui system call. Jika shell crash, kernel tetap aman.
 *
 * Syscall yang digunakan:
 *   5  (clear_screen)  - bersihkan layar
 *   6  (print_at)      - cetak teks pada posisi (x,y)
 *   7  (draw_char)     - gambar 1 karakter
 *   3  (read_key)      - baca keyboard
 *   4  (sleep)         - delay
 *   8  (read_file)     - baca file dari ramdisk
 *   9  (exec)          - jalankan program ELF
 *   10 (fill_rect)     - isi area persegi (untuk membersihkan baris)
 *   2  (exit)          - keluar
 */
#include "../libc/stdio.h"
#include "../libc/stdlib.h"
#include "../libc/string.h"

/* === Geometri Terminal ===
 * Font 8x8 dengan scale=2 → 1 cell = 16x16 piksel.
 * Tinggi baris 24 piksel (16 cell + 8 piksel jarak antar baris).
 * Margin kiri 50 piksel, area gambar dibatasi sampai x = 800.
 */
#define TERM_LEFT     50
#define TERM_TOP      50
#define TERM_RIGHT    800
#define TERM_LINE_H   24
#define TERM_CELL_W   16
#define TERM_BG       0x002244

/* Posisi kursor terminal (dikelola di user-space) */
static int cursor_x = TERM_LEFT;
static int cursor_y = TERM_TOP;

/*
 * BUG FIX: Bersihkan area baris (rect) sebelum menulis teks.
 * Tanpa ini, ketika perintah dijalankan berulang kali, teks lama
 * di posisi (cursor_x, cursor_y) tetap terlihat di sela-sela glyph
 * baru karena font 8x8 tidak menutupi seluruh cell secara opaque
 * dan jarak antar baris (8 px) tidak ikut tertimpa.
 */
static void terminal_clear_line(int y) {
    fill_rect(TERM_LEFT, y, TERM_RIGHT - TERM_LEFT, TERM_LINE_H, TERM_BG);
}

/* Cetak teks lalu pindah kursor ke baris berikutnya */
static void terminal_print(const char* text, uint32_t color) {
    terminal_clear_line(cursor_y);
    print_at(text, cursor_x, cursor_y, color);
    cursor_y += TERM_LINE_H;
    cursor_x = TERM_LEFT;
}

/* Cetak prompt shell */
static void print_prompt(void) {
    terminal_clear_line(cursor_y);
    print_at("Mandor@GenOS:~$ ", cursor_x, cursor_y, 0x00FFFF);
    cursor_x += 16 * TERM_CELL_W; /* Lebar prompt = 16 karakter × 16 piksel */
}

void _start(void) {
    /* --- LAYAR SELAMAT DATANG --- */
    clear_screen();
    terminal_print("=============================================", 0xFFFFFF);
    terminal_print("           GENOS SYSTEM TERMINAL V3          ", 0x00FF00);
    terminal_print("        [Ring 3 - Isolated User Space]       ", 0x00CC00);
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
                cursor_y += TERM_LINE_H;
                cursor_x = TERM_LEFT;

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
                        cursor_x = TERM_LEFT;
                        cursor_y = TERM_TOP;
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
                    /* --- RUN ---
                     *
                     * BUG FIX #17: Tunggu child app selesai dengan polling
                     * `wait_pid` di Ring 3 (bukan blocking di kernel — yang
                     * tidak aman karena kernel_stack_top di-share antar
                     * syscall). Setelah child mati, layar masih berisi
                     * tulisan app pada koordinat absolut, jadi kita
                     * bersihkan layar dan reset cursor agar prompt baru
                     * muncul rapi tanpa tertimpa output app.
                     */
                    else if (strcmp(cmd_buffer, "run") == 0) {
                        terminal_print("Loading app.elf into Ring 3...", 0x00FF00);
                        int pid = exec("app.elf");
                        if (pid <= 0) {
                            terminal_print("[ERROR] app.elf not found!", 0xFF0000);
                        } else {
                            /* Polling sampai child task DEAD. user_sleep
                             * memberi CPU ke scheduler agar child mendapat
                             * giliran berjalan. */
                            while (!wait_pid(pid)) {
                                user_sleep(50);
                            }
                            /* App selesai — bersihkan layar & reset terminal */
                            clear_screen();
                            cursor_x = TERM_LEFT;
                            cursor_y = TERM_TOP;
                            terminal_print("[app.elf finished - terminal restored]", 0x00FFFF);
                        }
                    }
                    /* --- COMMAND NOT FOUND --- */
                    else {
                        terminal_clear_line(cursor_y);
                        print_at("Command not found: ", cursor_x, cursor_y, 0xFF0000);
                        print_at(cmd_buffer, cursor_x + 260, cursor_y, 0xFFFF00);
                        cursor_y += TERM_LINE_H;
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
                    cursor_x -= TERM_CELL_W;
                    /*
                     * BUG FIX: gunakan fill_rect agar piksel di gap antar
                     * cell juga tertimpa, mencegah artefak karakter lama.
                     */
                    fill_rect(cursor_x, cursor_y, TERM_CELL_W, TERM_LINE_H, TERM_BG);
                }
            }
            /* ===== KARAKTER BIASA: Tampilkan dan simpan ===== */
            else {
                if (cmd_index < 254) {
                    cmd_buffer[cmd_index++] = c;
                    /*
                     * Bersihkan cell tujuan dulu agar tidak ada residu
                     * piksel dari karakter sebelumnya (mis. setelah backspace
                     * berulang lalu ketik karakter baru di kolom yang sama).
                     */
                    fill_rect(cursor_x, cursor_y, TERM_CELL_W, TERM_LINE_H, TERM_BG);
                    draw_char(c, cursor_x, cursor_y, 0xFFFFFF);
                    cursor_x += TERM_CELL_W;
                    if (cursor_x > 750) {
                        cursor_y += TERM_LINE_H;
                        cursor_x = prompt_batas_kiri;
                    }
                }
            }
        }
        else {
            /*
             * Tidak ada input — langsung loop kembali ke read_key().
             *
             * CATATAN: Kita TIDAK menggunakan user_sleep() di sini karena
             * sleep syscall melakukan sti;hlt di dalam kernel, yang konflik
             * dengan scheduler saat context-switch. Sebagai gantinya, shell
             * melakukan polling cepat. Keyboard IRQ tetap menyala di jendela
             * singkat antara sysretq dan syscall berikutnya (saat shell di
             * Ring 3 dengan IF=1).
             */
        }
    }
}

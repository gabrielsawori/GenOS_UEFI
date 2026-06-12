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
                        terminal_print("  ls    : List files (ramdisk + tmpfs)", 0xFFFFFF);
                        terminal_print("  read  : Read pesan.txt from Ramdisk", 0xFFFFFF);
                        terminal_print("  cat   : Read any file (TAR or tmpfs)", 0xFFFFFF);
                        terminal_print("  write : Write text to tmpfs file", 0xFFFFFF);
                        terminal_print("  rm    : Delete a tmpfs file", 0xFFFFFF);
                        terminal_print("  shm   : Shared memory IPC demo", 0xFFFFFF);
                        terminal_print("  cache : Buffer cache statistics", 0xFFFFFF);
                        terminal_print("  fork  : Fork process demo", 0xFFFFFF);
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
                    /* --- LS: List files via VFS readdir --- */
                    else if (strcmp(cmd_buffer, "ls") == 0) {
                        terminal_print("Ramdisk contents:", 0xFFFF00);
                        int dir_fd = open("/", 0);
                        if (dir_fd < 0) {
                            terminal_print("[ERROR] Cannot open ramdisk!", 0xFF0000);
                        } else {
                            char name_buf[100];
                            long size_val = 0;
                            while (readdir(dir_fd, name_buf, (int*)&size_val)) {
                                /* Format: "  filename (size bytes)" */
                                char line[200];
                                int j = 0;
                                line[j++] = ' '; line[j++] = ' ';
                                for (int k = 0; name_buf[k] && j < 150; k++) {
                                    line[j++] = name_buf[k];
                                }
                                line[j++] = ' '; line[j++] = '(';
                                /* Convert size to string */
                                if (size_val == 0) {
                                    line[j++] = '0';
                                } else {
                                    char num[16];
                                    int ni = 0;
                                    long tmp = size_val;
                                    while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; }
                                    for (int k = ni - 1; k >= 0; k--) line[j++] = num[k];
                                }
                                line[j++] = ' '; line[j++] = 'b'; line[j++] = 'y';
                                line[j++] = 't'; line[j++] = 'e'; line[j++] = 's';
                                line[j++] = ')'; line[j] = '\0';
                                terminal_print(line, 0xFFFFFF);
                                size_val = 0;
                            }
                            close(dir_fd);
                        }
                    }
                    /* --- READ: Use VFS open/read/close --- */
                    else if (strcmp(cmd_buffer, "read") == 0) {
                        int fd = open("pesan.txt", 0);
                        if (fd < 0) {
                            terminal_print("[ERROR] pesan.txt not found!", 0xFF0000);
                        } else {
                            char file_buf[256];
                            int bytes = read(fd, file_buf, 255);
                            close(fd);
                            if (bytes > 0) {
                                file_buf[bytes] = '\0';
                                terminal_print("Extracting pesan.txt via VFS:", 0xFFFF00);
                                terminal_print(file_buf, 0xFFFFFF);
                            } else {
                                terminal_print("[ERROR] pesan.txt is empty!", 0xFF0000);
                            }
                        }
                    }
                    /* --- RUN ---
                     *
                     * wait_pid sekarang BLOCKING: shell tidur (TASK_WAITING)
                     * sampai child mati, memberi CPU penuh ke app tanpa
                     * polling busy-wait. Setelah child mati, reaper
                     * membangunkan shell dan kita bersihkan layar.
                     */
                    else if (strcmp(cmd_buffer, "run") == 0) {
                        terminal_print("Loading app.elf into Ring 3...", 0x00FF00);
                        int pid = exec("app.elf");
                        if (pid <= 0) {
                            terminal_print("[ERROR] app.elf not found!", 0xFF0000);
                        } else {
                            /* Blokir sampai child task DEAD */
                            wait_pid(pid);
                            /* App selesai — bersihkan layar & reset terminal */
                            clear_screen();
                            cursor_x = TERM_LEFT;
                            cursor_y = TERM_TOP;
                            terminal_print("[app.elf finished - terminal restored]", 0x00FFFF);
                        }
                    }
                    /* --- SHM: Shared Memory IPC demo --- */
                    else if (strcmp(cmd_buffer, "shm") == 0) {
                        terminal_print("=== Shared Memory IPC Demo ===", 0xFFFF00);
                        /* 1. Create a 4096-byte segment */
                        int shmid = shm_create(4096);
                        if (shmid < 0) {
                            terminal_print("[ERROR] shm_create failed!", 0xFF0000);
                        } else {
                            /* 2. Attach it */
                            char* shm = (char*)shm_attach(shmid);
                            if (!shm) {
                                terminal_print("[ERROR] shm_attach failed!", 0xFF0000);
                            } else {
                                /* 3. Write a message to shared memory */
                                const char* msg = "Hello from shared memory!";
                                int i;
                                for (i = 0; msg[i]; i++) shm[i] = msg[i];
                                shm[i] = '\0';
                                terminal_print("Wrote to SHM: ", 0x00CCFF);
                                terminal_print(shm, 0xFFFFFF);
                                /* 4. Read it back */
                                terminal_print("Read from SHM: ", 0x00CCFF);
                                terminal_print(shm, 0x00FF00);
                                /* 5. Detach */
                                shm_detach(shm);
                                terminal_print("[OK] Detached.", 0x00CCFF);
                            }
                            /* 6. Destroy */
                            shm_destroy(shmid);
                            terminal_print("[OK] Segment destroyed.", 0x00CCFF);
                        }
                    }
                    /* --- CACHE: Buffer Cache Statistics --- */
                    else if (strcmp(cmd_buffer, "cache") == 0) {
                        terminal_print("=== Buffer Cache Statistics ===", 0xFFFF00);
                        cache_stats_t cs;
                        if (cache_get_stats(&cs) == 0) {
                            char line[128];

                            /* Helper: uint64 to string */
                            #define U64_STR(val, buf, len) do { \
                                char _t[24]; int _n = 0; \
                                uint64_t _v = (val); \
                                if (_v == 0) { _t[_n++] = '0'; } \
                                else { while (_v > 0) { _t[_n++] = '0' + (_v % 10); _v /= 10; } } \
                                for (int _k = _n - 1; _k >= 0 && (len) < 100; _k--) (buf)[(len)++] = _t[_k]; \
                            } while(0)

                            /* Hits */
                            int j = 0;
                            const char* h1 = "  Hits:      ";
                            for (int k = 0; h1[k]; k++) line[j++] = h1[k];
                            U64_STR(cs.hits, line, j);
                            line[j] = '\0';
                            terminal_print(line, 0x00FF00);

                            /* Misses */
                            j = 0;
                            const char* h2 = "  Misses:    ";
                            for (int k = 0; h2[k]; k++) line[j++] = h2[k];
                            U64_STR(cs.misses, line, j);
                            line[j] = '\0';
                            terminal_print(line, 0xFF6600);

                            /* Evictions */
                            j = 0;
                            const char* h3 = "  Evictions: ";
                            for (int k = 0; h3[k]; k++) line[j++] = h3[k];
                            U64_STR(cs.evictions, line, j);
                            line[j] = '\0';
                            terminal_print(line, 0xFF0000);

                            /* Blocks used / total */
                            j = 0;
                            const char* h4 = "  Blocks:    ";
                            for (int k = 0; h4[k]; k++) line[j++] = h4[k];
                            U64_STR(cs.used_blocks, line, j);
                            line[j++] = ' '; line[j++] = '/'; line[j++] = ' ';
                            U64_STR(cs.total_blocks, line, j);
                            line[j++] = ' '; line[j++] = '(';
                            /* Calculate used KB */
                            uint64_t used_kb = (uint64_t)cs.used_blocks * 4;
                            U64_STR(used_kb, line, j);
                            line[j++] = 'K'; line[j++] = 'B';
                            line[j++] = ')'; line[j] = '\0';
                            terminal_print(line, 0x00CCFF);

                            /* Hit rate */
                            uint64_t total = cs.hits + cs.misses;
                            j = 0;
                            const char* h5 = "  Hit Rate:  ";
                            for (int k = 0; h5[k]; k++) line[j++] = h5[k];
                            if (total > 0) {
                                uint64_t pct = (cs.hits * 100) / total;
                                U64_STR(pct, line, j);
                                line[j++] = '%';
                            } else {
                                line[j++] = 'N'; line[j++] = '/'; line[j++] = 'A';
                            }
                            line[j] = '\0';
                            terminal_print(line, 0xFFFFFF);

                            #undef U64_STR
                        } else {
                            terminal_print("[ERROR] Failed to get cache stats!", 0xFF0000);
                        }
                    }
                    /* --- FORK: Process cloning demo --- */
                    else if (strcmp(cmd_buffer, "fork") == 0) {
                        terminal_print("=== Fork Demo ===", 0xFFFF00);
                        int child_pid = fork();
                        if (child_pid < 0) {
                            terminal_print("[ERROR] fork() failed!", 0xFF0000);
                        } else if (child_pid == 0) {
                            /* We are the CHILD process */
                            terminal_print("[CHILD] I am the cloned process!", 0x00FF00);
                            terminal_print("[CHILD] Exiting now...", 0x00FF00);
                            exit(0);
                        } else {
                            /* We are the PARENT process */
                            char line[80];
                            int j = 0;
                            const char* prefix = "[PARENT] Child created with PID: ";
                            for (int k = 0; prefix[k]; k++) line[j++] = prefix[k];
                            /* Convert PID to string */
                            char num[16];
                            int ni = 0;
                            int tmp = child_pid;
                            if (tmp == 0) { num[ni++] = '0'; }
                            else { while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; } }
                            for (int k = ni - 1; k >= 0; k--) line[j++] = num[k];
                            line[j] = '\0';
                            terminal_print(line, 0x00CCFF);
                            /* Wait for child to finish */
                            wait_pid(child_pid);
                            terminal_print("[PARENT] Child finished.", 0x00CCFF);
                        }
                    }
                    /* --- WRITE: Write text to tmpfs file --- */
                    else if (cmd_buffer[0] == 'w' && cmd_buffer[1] == 'r' && cmd_buffer[2] == 'i' && 
                             cmd_buffer[3] == 't' && cmd_buffer[4] == 'e' && cmd_buffer[5] == ' ') {
                        /* Parse: write <filename> <text> */
                        char* ptr = &cmd_buffer[6];
                        char fname[64];
                        int fi = 0;
                        while (*ptr && *ptr != ' ' && fi < 63) {
                            fname[fi++] = *ptr++;
                        }
                        fname[fi] = '\0';
                        
                        if (*ptr == ' ') ptr++; /* Skip space */
                        
                        if (fi == 0) {
                            terminal_print("Usage: write <filename> <text>", 0xFF0000);
                        } else {
                            /* Create or open file */
                            int fd = open(fname, 4); /* O_CREATE */
                            if (fd < 0) {
                                terminal_print("[ERROR] Cannot create file", 0xFF0000);
                            } else {
                                /* Calculate text length */
                                int len = 0;
                                while (ptr[len]) len++;
                                
                                int written = write(fd, ptr, len);
                                close(fd);
                                
                                if (written > 0) {
                                    terminal_print("[OK] Wrote to tmpfs: ", 0x00FF00);
                                    terminal_print(fname, 0xFFFFFF);
                                } else {
                                    terminal_print("[ERROR] Write failed", 0xFF0000);
                                }
                            }
                        }
                    }
                    /* --- CAT: Read and display any file --- */
                    else if (cmd_buffer[0] == 'c' && cmd_buffer[1] == 'a' && cmd_buffer[2] == 't' && cmd_buffer[3] == ' ') {
                        char* fname = &cmd_buffer[4];
                        int fd = open(fname, 0);
                        if (fd < 0) {
                            terminal_print("[ERROR] File not found: ", 0xFF0000);
                            terminal_print(fname, 0xFFFF00);
                        } else {
                            char file_buf[512];
                            int bytes = read(fd, file_buf, 511);
                            close(fd);
                            if (bytes > 0) {
                                file_buf[bytes] = '\0';
                                terminal_print("--- File content ---", 0xFFFF00);
                                terminal_print(file_buf, 0xFFFFFF);
                                terminal_print("--- End of file ---", 0xFFFF00);
                            } else {
                                terminal_print("[INFO] File is empty", 0xAAAAAA);
                            }
                        }
                    }
                    /* --- RM: Delete a tmpfs file --- */
                    else if (cmd_buffer[0] == 'r' && cmd_buffer[1] == 'm' && cmd_buffer[2] == ' ') {
                        char* fname = &cmd_buffer[3];
                        int ret = unlink(fname);
                        if (ret == 0) {
                            terminal_print("[OK] Deleted: ", 0x00FF00);
                            terminal_print(fname, 0xFFFFFF);
                        } else {
                            terminal_print("[ERROR] Cannot delete: ", 0xFF0000);
                            terminal_print(fname, 0xFFFF00);
                            terminal_print("(only tmpfs files can be deleted)", 0xAAAAAA);
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

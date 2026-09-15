/*
 * GenOS v4 Security Terminal (Ring 3 — User Space)
 *
 * Terminal berjalan sepenuhnya di Ring 3 dengan fitur:
 *   - Scroll buffer 256 baris history
 *   - Page Up/Down untuk navigasi history
 *   - Cybersecurity-themed UI
 *   - Status bar dengan info sistem
 *
 * Semua komunikasi dengan kernel melalui system call.
 */
#include "../libc/stdio.h"
#include "../libc/stdlib.h"
#include "../libc/string.h"
#include "../libc/crypto.h"

/* === Special Key Codes (matching keyboard driver) === */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_PGUP  0x84
#define KEY_PGDN  0x85

/* === Cyber Security Theme === */
#define COLOR_BG        0x0A0E14
#define COLOR_TEXT       0x00FF41
#define COLOR_PROMPT     0x00FFFF
#define COLOR_ERROR      0xFF3333
#define COLOR_WARN       0xFFCC00
#define COLOR_SUCCESS    0x33FF33
#define COLOR_INFO       0x00AAFF
#define COLOR_HEADER     0xFFFFFF
#define COLOR_DIM        0x556677
#define COLOR_STATUS_BG  0x16192B
#define COLOR_STATUS_FG  0x8888AA
#define COLOR_ACCENT     0xFF6600

/* === Terminal Geometry === */
#define TERM_LEFT    24
#define TERM_CELL_W  16
#define TERM_LINE_H  22
#define STATUS_BAR_H 28

/* === Scroll Buffer === */
#define SCROLL_LINES 256
#define SCROLL_COLS  120

typedef struct {
    char text[SCROLL_COLS + 1];
    uint32_t color;
} terminal_line_t;

static terminal_line_t scroll_buf[SCROLL_LINES];
static int buf_head  = 0;
static int buf_count = 0;
static int scroll_offset = 0;

/* === Screen State === */
static int screen_w = 1024;
static int screen_h = 768;
static int visible_lines = 28;
static int term_top = 36;
static int chars_per_line = 60;
static int cursor_x;
static int input_y;

/* === Number-to-string helper === */
static void u64_to_str(uint64_t val, char* buf, int* pos) {
    char tmp[24];
    int n = 0;
    if (val == 0) { tmp[n++] = '0'; }
    else { while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; } }
    for (int k = n - 1; k >= 0; k--) buf[(*pos)++] = tmp[k];
}

static void i32_to_str(int val, char* buf, int* pos) {
    if (val < 0) { buf[(*pos)++] = '-'; val = -val; }
    u64_to_str((uint64_t)(unsigned int)val, buf, pos);
}

static void str_append(char* buf, int* pos, const char* s) {
    while (*s && *pos < 120) buf[(*pos)++] = *s++;
}

/* === Status Bar === */
static void render_status_bar(void) {
    fill_rect(0, 0, screen_w, STATUS_BAR_H, COLOR_STATUS_BG);

    /* Left: branding */
    print_at(" GenOS v4", 8, 6, COLOR_ACCENT);
    print_at(" Security Terminal", 8 + 9 * TERM_CELL_W, 6, COLOR_STATUS_FG);

    /* Right: scroll indicator */
    if (scroll_offset > 0) {
        char info[32];
        int j = 0;
        str_append(info, &j, "[SCROLL ");
        i32_to_str(scroll_offset, info, &j);
        info[j++] = ']';
        info[j] = '\0';
        print_at(info, screen_w - (j + 1) * TERM_CELL_W, 6, COLOR_WARN);
    }
}

/* === Viewport Renderer === */
static void render_viewport(void) {
    /* Clear terminal area */
    fill_rect(0, term_top, screen_w, screen_h - term_top, COLOR_BG);

    /* When scroll_offset > 0 (viewing history), use full area.
     * When at bottom, reserve last line for input prompt. */
    int max_lines = (scroll_offset > 0) ? visible_lines : visible_lines - 1;

    /* Calculate first line index to display */
    int first_line = buf_count - scroll_offset - max_lines;
    if (first_line < 0) first_line = 0;

    int oldest_pos = (buf_head - buf_count + SCROLL_LINES) % SCROLL_LINES;
    if (oldest_pos < 0) oldest_pos += SCROLL_LINES;

    int end_line = buf_count - scroll_offset;
    int lines_drawn = 0;

    for (int i = first_line; i < end_line && lines_drawn < max_lines; i++, lines_drawn++) {
        int pos = (oldest_pos + i) % SCROLL_LINES;
        int y = term_top + lines_drawn * TERM_LINE_H;
        print_at(scroll_buf[pos].text, TERM_LEFT, y, scroll_buf[pos].color);
    }

    /* Input line Y position */
    input_y = term_top + lines_drawn * TERM_LINE_H;

    /* Status bar (update scroll indicator) */
    render_status_bar();
}

/* === Terminal Output === */
static void terminal_print(const char* text, uint32_t color) {
    int i;
    for (i = 0; i < SCROLL_COLS && text[i]; i++)
        scroll_buf[buf_head].text[i] = text[i];
    scroll_buf[buf_head].text[i] = '\0';
    scroll_buf[buf_head].color = color;

    buf_head = (buf_head + 1) % SCROLL_LINES;
    if (buf_count < SCROLL_LINES) buf_count++;

    /* Auto-scroll to bottom on new output */
    scroll_offset = 0;

    render_viewport();
}

/* === Prompt === */
static int prompt_len = 0;

static void print_prompt(void) {
    char uname[32];
    crypto_whoami(uname, sizeof(uname));

    char prompt[64];
    int j = 0;
    for (int k = 0; uname[k] && j < 28; k++) prompt[j++] = uname[k];
    str_append(prompt, &j, "@GenOS:~$ ");
    prompt[j] = '\0';
    prompt_len = j;

    fill_rect(TERM_LEFT, input_y, screen_w - TERM_LEFT, TERM_LINE_H, COLOR_BG);
    print_at(prompt, TERM_LEFT, input_y, COLOR_PROMPT);
    cursor_x = TERM_LEFT + j * TERM_CELL_W;
}

/* === Welcome Screen === */
static void show_welcome(void) {
    terminal_print("", COLOR_TEXT);
    terminal_print("  ======================================================", COLOR_DIM);
    terminal_print("", COLOR_TEXT);
    terminal_print("       G E N O S   v 4   S E C U R I T Y   K E R N E L", COLOR_ACCENT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  ======================================================", COLOR_DIM);
    terminal_print("", COLOR_TEXT);
    terminal_print("  [*] Ring 3 Isolated Terminal", COLOR_TEXT);
    terminal_print("  [*] SMEP + NX/W^X Enforced", COLOR_TEXT);
    terminal_print("  [*] User Pointer Validation Active", COLOR_TEXT);
    terminal_print("  [*] AES-256 / SHA-256 / HMAC / PBKDF2", COLOR_TEXT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  Type 'help' for commands.  Page Up/Down to scroll.", COLOR_DIM);
    terminal_print("", COLOR_TEXT);
}

/* ================================================================
 *                     COMMAND HANDLERS
 * ================================================================ */

static void cmd_help(void) {
    terminal_print("=== Available Commands ===", COLOR_INFO);
    terminal_print("", COLOR_TEXT);
    terminal_print("  System:", COLOR_WARN);
    terminal_print("    help      Show this message", COLOR_TEXT);
    terminal_print("    clear     Clear terminal + history", COLOR_TEXT);
    terminal_print("    info      System information", COLOR_TEXT);
    terminal_print("    run       Execute app.elf", COLOR_TEXT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  Filesystem:", COLOR_WARN);
    terminal_print("    ls        List files (ramdisk + tmpfs)", COLOR_TEXT);
    terminal_print("    cat <f>   Read file contents", COLOR_TEXT);
    terminal_print("    read      Read pesan.txt", COLOR_TEXT);
    terminal_print("    write <f> <text>   Write to tmpfs", COLOR_TEXT);
    terminal_print("    rm <f>    Delete tmpfs file", COLOR_TEXT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  Security:", COLOR_WARN);
    terminal_print("    whoami    Current user info", COLOR_TEXT);
    terminal_print("    login <u> Authenticate as user", COLOR_TEXT);
    terminal_print("    hash <t>  SHA-256 hash", COLOR_TEXT);
    terminal_print("    random    Generate 128-bit random", COLOR_TEXT);
    terminal_print("    encrypt <t>  AES-256-CBC demo", COLOR_TEXT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  IPC & Process:", COLOR_WARN);
    terminal_print("    shm       Shared memory demo", COLOR_TEXT);
    terminal_print("    cache     Buffer cache stats", COLOR_TEXT);
    terminal_print("    fork      Fork process demo", COLOR_TEXT);
    terminal_print("", COLOR_TEXT);
    terminal_print("  Power:", COLOR_WARN);
    terminal_print("    shutdown  Power off", COLOR_TEXT);
    terminal_print("    restart   Reboot system", COLOR_TEXT);
}

static void cmd_info(void) {
    terminal_print("[*] GenOS v4 Security Kernel (64-bit UEFI)", COLOR_SUCCESS);
    terminal_print("[*] Architecture: x86_64 Long Mode", COLOR_TEXT);
    terminal_print("[*] Isolation: Ring-3 User Space Terminal", COLOR_TEXT);
    terminal_print("[*] Security: SMEP + NX/W^X + Pointer Validation", COLOR_TEXT);
    terminal_print("[*] Crypto: AES-256-CBC, SHA-256, HMAC, PBKDF2", COLOR_TEXT);
}

static void cmd_ls(void) {
    terminal_print("=== File Listing ===", COLOR_INFO);
    int dir_fd = open("/", 0);
    if (dir_fd < 0) { terminal_print("[ERROR] Cannot open ramdisk!", COLOR_ERROR); return; }
    char name_buf[100];
    long size_val = 0;
    while (readdir(dir_fd, name_buf, (int*)&size_val)) {
        char line[200];
        int j = 0;
        str_append(line, &j, "  ");
        str_append(line, &j, name_buf);
        /* Pad to 30 chars */
        while (j < 30) line[j++] = ' ';
        u64_to_str((uint64_t)size_val, line, &j);
        str_append(line, &j, " bytes");
        line[j] = '\0';
        terminal_print(line, COLOR_TEXT);
        size_val = 0;
    }
    close(dir_fd);
}

static void cmd_read(void) {
    int fd = open("pesan.txt", 0);
    if (fd < 0) { terminal_print("[ERROR] pesan.txt not found!", COLOR_ERROR); return; }
    char file_buf[256];
    int bytes = read(fd, file_buf, 255);
    close(fd);
    if (bytes > 0) {
        file_buf[bytes] = '\0';
        terminal_print("[pesan.txt]:", COLOR_INFO);
        terminal_print(file_buf, COLOR_TEXT);
    } else {
        terminal_print("[INFO] File is empty", COLOR_DIM);
    }
}

static void cmd_cat(const char* fname) {
    int fd = open(fname, 0);
    if (fd < 0) {
        char line[128]; int j = 0;
        str_append(line, &j, "[ERROR] File not found: ");
        str_append(line, &j, fname);
        line[j] = '\0';
        terminal_print(line, COLOR_ERROR);
        return;
    }
    char file_buf[512];
    int bytes = read(fd, file_buf, 511);
    close(fd);
    if (bytes > 0) {
        file_buf[bytes] = '\0';
        terminal_print(file_buf, COLOR_TEXT);
    } else {
        terminal_print("[INFO] File is empty", COLOR_DIM);
    }
}

static void cmd_write(const char* args) {
    char fname[64];
    int fi = 0;
    while (*args && *args != ' ' && fi < 63) fname[fi++] = *args++;
    fname[fi] = '\0';
    if (*args == ' ') args++;
    if (fi == 0) { terminal_print("Usage: write <filename> <text>", COLOR_ERROR); return; }

    int fd = open(fname, 4);
    if (fd < 0) { terminal_print("[ERROR] Cannot create file", COLOR_ERROR); return; }
    int len = 0; while (args[len]) len++;
    int written = write(fd, args, len);
    close(fd);
    if (written > 0) {
        char line[128]; int j = 0;
        str_append(line, &j, "[OK] Wrote ");
        i32_to_str(written, line, &j);
        str_append(line, &j, " bytes to ");
        str_append(line, &j, fname);
        line[j] = '\0';
        terminal_print(line, COLOR_SUCCESS);
    } else {
        terminal_print("[ERROR] Write failed", COLOR_ERROR);
    }
}

static void cmd_rm(const char* fname) {
    int ret = unlink(fname);
    if (ret == 0) {
        char line[80]; int j = 0;
        str_append(line, &j, "[OK] Deleted: ");
        str_append(line, &j, fname);
        line[j] = '\0';
        terminal_print(line, COLOR_SUCCESS);
    } else {
        terminal_print("[ERROR] Cannot delete (only tmpfs)", COLOR_ERROR);
    }
}

static void cmd_run(void) {
    terminal_print("[*] Loading app.elf...", COLOR_INFO);
    int pid = exec("app.elf");
    if (pid <= 0) { terminal_print("[ERROR] app.elf not found!", COLOR_ERROR); return; }
    wait_pid(pid);
    clear_screen();
    fill_rect(0, 0, screen_w, screen_h, COLOR_BG);
    render_status_bar();
    render_viewport();
    terminal_print("[app.elf finished - terminal restored]", COLOR_INFO);
}

static void cmd_shm(void) {
    terminal_print("=== Shared Memory IPC Demo ===", COLOR_INFO);
    int shmid = shm_create(4096);
    if (shmid < 0) { terminal_print("[ERROR] shm_create failed!", COLOR_ERROR); return; }
    char* shm = (char*)shm_attach(shmid);
    if (!shm) { terminal_print("[ERROR] shm_attach failed!", COLOR_ERROR); return; }
    const char* msg = "Hello from shared memory!";
    int i; for (i = 0; msg[i]; i++) shm[i] = msg[i]; shm[i] = '\0';

    char line[80]; int j = 0;
    str_append(line, &j, "  Write: ");
    str_append(line, &j, shm);
    line[j] = '\0';
    terminal_print(line, COLOR_TEXT);

    j = 0;
    str_append(line, &j, "  Read:  ");
    str_append(line, &j, shm);
    line[j] = '\0';
    terminal_print(line, COLOR_SUCCESS);

    shm_detach(shm);
    shm_destroy(shmid);
    terminal_print("[OK] Segment destroyed.", COLOR_DIM);
}

static void cmd_cache(void) {
    terminal_print("=== Buffer Cache Statistics ===", COLOR_INFO);
    cache_stats_t cs;
    if (cache_get_stats(&cs) != 0) { terminal_print("[ERROR] Failed!", COLOR_ERROR); return; }

    char line[128]; int j;

    j = 0; str_append(line, &j, "  Hits:       "); u64_to_str(cs.hits, line, &j); line[j]=0;
    terminal_print(line, COLOR_SUCCESS);
    j = 0; str_append(line, &j, "  Misses:     "); u64_to_str(cs.misses, line, &j); line[j]=0;
    terminal_print(line, COLOR_ACCENT);
    j = 0; str_append(line, &j, "  Evictions:  "); u64_to_str(cs.evictions, line, &j); line[j]=0;
    terminal_print(line, COLOR_ERROR);
    j = 0; str_append(line, &j, "  Blocks:     "); u64_to_str(cs.used_blocks, line, &j);
    str_append(line, &j, " / "); u64_to_str(cs.total_blocks, line, &j); line[j]=0;
    terminal_print(line, COLOR_TEXT);

    uint64_t total = cs.hits + cs.misses;
    j = 0; str_append(line, &j, "  Hit Rate:   ");
    if (total > 0) { u64_to_str((cs.hits * 100) / total, line, &j); line[j++] = '%'; }
    else { str_append(line, &j, "N/A"); }
    line[j] = 0;
    terminal_print(line, COLOR_HEADER);
}

static void cmd_fork(void) {
    terminal_print("=== Fork Demo ===", COLOR_INFO);
    int child_pid = fork();
    if (child_pid < 0) { terminal_print("[ERROR] fork() failed!", COLOR_ERROR); }
    else if (child_pid == 0) {
        terminal_print("[CHILD] I am the cloned process!", COLOR_SUCCESS);
        terminal_print("[CHILD] Exiting...", COLOR_SUCCESS);
        exit(0);
    } else {
        char line[80]; int j = 0;
        str_append(line, &j, "[PARENT] Child PID: ");
        i32_to_str(child_pid, line, &j);
        line[j] = '\0';
        terminal_print(line, COLOR_TEXT);
        wait_pid(child_pid);
        terminal_print("[PARENT] Child finished.", COLOR_DIM);
    }
}

static void cmd_whoami(void) {
    char uname[32];
    int uid = crypto_whoami(uname, sizeof(uname));
    char line[80]; int j = 0;
    str_append(line, &j, "[*] User: ");
    str_append(line, &j, uname);
    str_append(line, &j, "  (UID ");
    i32_to_str(uid, line, &j);
    line[j++] = ')'; line[j] = '\0';
    terminal_print(line, COLOR_SUCCESS);
}

static void cmd_login(const char* uname) {
    terminal_print("Password: ", COLOR_DIM);
    /* Render password prompt on input line */
    fill_rect(TERM_LEFT, input_y, screen_w - TERM_LEFT, TERM_LINE_H, COLOR_BG);
    print_at("Password: ", TERM_LEFT, input_y, COLOR_DIM);
    int px = TERM_LEFT + 10 * TERM_CELL_W;

    char pass[64]; int pi = 0;
    while (1) {
        char k = read_key();
        if (k == '\n') break;
        if (k == '\b' && pi > 0) {
            pi--;
            px -= TERM_CELL_W;
            fill_rect(px, input_y, TERM_CELL_W, TERM_LINE_H, COLOR_BG);
            continue;
        }
        if (k && k != '\b' && pi < 63) {
            pass[pi++] = k;
            draw_char('*', px, input_y, COLOR_DIM);
            px += TERM_CELL_W;
        }
    }
    pass[pi] = '\0';

    int uid = crypto_login(uname, pass);
    if (uid >= 0) {
        char line[80]; int j = 0;
        str_append(line, &j, "[OK] Authenticated as ");
        str_append(line, &j, uname);
        str_append(line, &j, " (UID ");
        i32_to_str(uid, line, &j);
        line[j++] = ')'; line[j] = '\0';
        terminal_print(line, COLOR_SUCCESS);
    } else {
        terminal_print("[FAIL] Authentication failed!", COLOR_ERROR);
    }
}

static void cmd_hash(const char* text) {
    int tlen = 0; while (text[tlen]) tlen++;
    uint8_t digest[32];
    crypto_sha256(text, (unsigned long)tlen, digest);
    const char* hc = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = hc[(digest[i]>>4)&0xf];
        hex[i*2+1] = hc[digest[i]&0xf];
    }
    hex[64] = '\0';
    terminal_print("[SHA-256]", COLOR_INFO);
    terminal_print(hex, COLOR_TEXT);
}

static void cmd_random(void) {
    uint8_t rbuf[16];
    crypto_random(rbuf, 16);
    const char* hc = "0123456789abcdef";
    char hex[48]; int j = 0;
    for (int i = 0; i < 16; i++) {
        hex[j++] = hc[(rbuf[i]>>4)&0xf];
        hex[j++] = hc[rbuf[i]&0xf];
        if (i % 4 == 3 && i < 15) hex[j++] = ' ';
    }
    hex[j] = '\0';
    terminal_print("[RANDOM 128-bit]", COLOR_INFO);
    terminal_print(hex, COLOR_SUCCESS);
}

static void cmd_encrypt(const char* text) {
    int tlen = 0; while (text[tlen]) tlen++;
    uint8_t key[32], iv[16];
    crypto_random(key, 32);
    crypto_random(iv, 16);
    uint8_t ct[256], pt[256];
    crypto_aes_params_t ep = {key, iv, text, (unsigned long)tlen, ct, sizeof(ct)};
    int ct_len = crypto_aes_encrypt(&ep);
    if (ct_len > 0) {
        const char* hc = "0123456789abcdef";
        char hex[64]; int j = 0;
        int show = ct_len > 16 ? 16 : ct_len;
        for (int i = 0; i < show; i++) {
            hex[j++] = hc[(ct[i]>>4)&0xf];
            hex[j++] = hc[ct[i]&0xf];
        }
        hex[j++]='.'; hex[j++]='.'; hex[j++]='.'; hex[j]='\0';
        terminal_print("[AES-256-CBC Ciphertext]", COLOR_INFO);
        terminal_print(hex, COLOR_ACCENT);

        crypto_aes_params_t dp = {key, iv, ct, (unsigned long)ct_len, pt, sizeof(pt)};
        int pt_len = crypto_aes_decrypt(&dp);
        if (pt_len > 0) {
            pt[pt_len] = '\0';
            terminal_print("[Decrypted]", COLOR_INFO);
            terminal_print((char*)pt, COLOR_SUCCESS);
        }
    } else {
        terminal_print("[ERROR] Encryption failed!", COLOR_ERROR);
    }
}

/* ================================================================
 *                     MAIN ENTRY POINT
 * ================================================================ */

void _start(void) {
    /* Initialize screen geometry */
    screen_info_t si;
    if (get_screen_info(&si) == 0) {
        screen_w = si.width;
        screen_h = si.height;
    }
    term_top = STATUS_BAR_H + 8;
    visible_lines = (screen_h - term_top - 16) / TERM_LINE_H;  /* 16px bottom margin */
    if (visible_lines > 40) visible_lines = 40;
    if (visible_lines < 10) visible_lines = 10;
    chars_per_line = (screen_w - TERM_LEFT * 2) / TERM_CELL_W;
    if (chars_per_line > SCROLL_COLS) chars_per_line = SCROLL_COLS;

    /* Clear screen with new theme */
    clear_screen();
    fill_rect(0, 0, screen_w, screen_h, COLOR_BG);
    render_status_bar();

    /* Welcome message */
    show_welcome();

    char cmd_buffer[256];
    int cmd_index = 0;

    print_prompt();

    /* === MAIN LOOP === */
    while (1) {
        char c = read_key();
        if (c == 0) continue;

        /* --- Page Up: Scroll history up --- */
        if (c == (char)KEY_PGUP) {
            int max_scroll = buf_count - (visible_lines - 1);
            if (max_scroll < 0) max_scroll = 0;
            scroll_offset += visible_lines / 2;
            if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            render_viewport();
            if (scroll_offset == 0) print_prompt();
            continue;
        }

        /* --- Page Down: Scroll history down --- */
        if (c == (char)KEY_PGDN) {
            scroll_offset -= visible_lines / 2;
            if (scroll_offset < 0) scroll_offset = 0;
            render_viewport();
            if (scroll_offset == 0) print_prompt();
            continue;
        }

        /* Auto-scroll to bottom on any other key */
        if (scroll_offset > 0) {
            scroll_offset = 0;
            render_viewport();
            print_prompt();
            /* Re-draw current input */
            for (int i = 0; i < cmd_index; i++) {
                int cx = TERM_LEFT + (prompt_len + i) * TERM_CELL_W;
                draw_char(cmd_buffer[i], cx, input_y, COLOR_HEADER);
            }
            cursor_x = TERM_LEFT + (prompt_len + cmd_index) * TERM_CELL_W;
        }

        /* --- ENTER: Execute command --- */
        if (c == '\n') {
            cmd_buffer[cmd_index] = '\0';

            /* Add typed command to scroll buffer */
            {
                char uname[32];
                crypto_whoami(uname, sizeof(uname));
                char prompt_line[200]; int j = 0;
                for (int k = 0; uname[k] && j < 28; k++) prompt_line[j++] = uname[k];
                str_append(prompt_line, &j, "@GenOS:~$ ");
                str_append(prompt_line, &j, cmd_buffer);
                prompt_line[j] = '\0';
                terminal_print(prompt_line, COLOR_PROMPT);
            }

            if (cmd_index > 0) {
                /* === COMMAND DISPATCH === */
                if (strcmp(cmd_buffer, "help") == 0)           cmd_help();
                else if (strcmp(cmd_buffer, "clear") == 0) {
                    buf_head = 0; buf_count = 0; scroll_offset = 0;
                    fill_rect(0, 0, screen_w, screen_h, COLOR_BG);
                    render_status_bar();
                    render_viewport();
                }
                else if (strcmp(cmd_buffer, "info") == 0)      cmd_info();
                else if (strcmp(cmd_buffer, "ls") == 0)         cmd_ls();
                else if (strcmp(cmd_buffer, "read") == 0)       cmd_read();
                else if (strcmp(cmd_buffer, "run") == 0)        cmd_run();
                else if (strcmp(cmd_buffer, "shm") == 0)        cmd_shm();
                else if (strcmp(cmd_buffer, "cache") == 0)      cmd_cache();
                else if (strcmp(cmd_buffer, "fork") == 0)       cmd_fork();
                else if (strcmp(cmd_buffer, "whoami") == 0)     cmd_whoami();
                else if (strcmp(cmd_buffer, "random") == 0)     cmd_random();
                else if (strcmp(cmd_buffer, "hash") == 0)
                    terminal_print("Usage: hash <text>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "encrypt") == 0)
                    terminal_print("Usage: encrypt <text>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "login") == 0)
                    terminal_print("Usage: login <username>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "cat") == 0)
                    terminal_print("Usage: cat <filename>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "write") == 0)
                    terminal_print("Usage: write <filename> <text>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "rm") == 0)
                    terminal_print("Usage: rm <filename>", COLOR_WARN);
                else if (strcmp(cmd_buffer, "shutdown") == 0)
                    power_shutdown();
                else if (strcmp(cmd_buffer, "restart") == 0)
                    power_restart();
                /* Commands with arguments */
                else if (cmd_buffer[0]=='c' && cmd_buffer[1]=='a' && cmd_buffer[2]=='t' && cmd_buffer[3]==' ')
                    cmd_cat(&cmd_buffer[4]);
                else if (cmd_buffer[0]=='w' && cmd_buffer[1]=='r' && cmd_buffer[2]=='i' &&
                         cmd_buffer[3]=='t' && cmd_buffer[4]=='e' && cmd_buffer[5]==' ')
                    cmd_write(&cmd_buffer[6]);
                else if (cmd_buffer[0]=='r' && cmd_buffer[1]=='m' && cmd_buffer[2]==' ')
                    cmd_rm(&cmd_buffer[3]);
                else if (cmd_buffer[0]=='l' && cmd_buffer[1]=='o' && cmd_buffer[2]=='g' &&
                         cmd_buffer[3]=='i' && cmd_buffer[4]=='n' && cmd_buffer[5]==' ')
                    cmd_login(&cmd_buffer[6]);
                else if (cmd_buffer[0]=='h' && cmd_buffer[1]=='a' && cmd_buffer[2]=='s' &&
                         cmd_buffer[3]=='h' && cmd_buffer[4]==' ')
                    cmd_hash(&cmd_buffer[5]);
                else if (cmd_buffer[0]=='e' && cmd_buffer[1]=='n' && cmd_buffer[2]=='c' &&
                         cmd_buffer[3]=='r' && cmd_buffer[4]=='y' && cmd_buffer[5]=='p' &&
                         cmd_buffer[6]=='t' && cmd_buffer[7]==' ')
                    cmd_encrypt(&cmd_buffer[8]);
                else {
                    char line[128]; int j = 0;
                    str_append(line, &j, "[?] Unknown command: ");
                    str_append(line, &j, cmd_buffer);
                    line[j] = '\0';
                    terminal_print(line, COLOR_ERROR);
                }
            }

            cmd_index = 0;
            print_prompt();
        }
        /* --- BACKSPACE --- */
        else if (c == '\b') {
            if (cmd_index > 0) {
                cmd_index--;
                cursor_x -= TERM_CELL_W;
                fill_rect(cursor_x, input_y, TERM_CELL_W, TERM_LINE_H, COLOR_BG);
            }
        }
        /* --- Ignore arrow keys in input for now --- */
        else if (c == (char)KEY_UP || c == (char)KEY_DOWN ||
                 c == (char)KEY_LEFT || c == (char)KEY_RIGHT) {
            /* Reserved for future command history */
        }
        /* --- Regular character --- */
        else if (cmd_index < 254) {
            cmd_buffer[cmd_index++] = c;
            fill_rect(cursor_x, input_y, TERM_CELL_W, TERM_LINE_H, COLOR_BG);
            draw_char(c, cursor_x, input_y, COLOR_HEADER);
            cursor_x += TERM_CELL_W;
        }
    }
}

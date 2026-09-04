/*
 * GenOS Desktop Environment (Ring 3 — User Space)
 *
 * OPTIMIZED: Menggunakan shared-memory back-buffer untuk rendering.
 * Semua operasi gambar (rect, char, text) menulis LANGSUNG ke RAM
 * tanpa syscall. Hanya flush_screen() di akhir frame yang melakukan
 * syscall (1× per frame, bukan 250+× seperti sebelumnya).
 *
 * Speedup: ~250× lebih sedikit syscall per frame.
 */
#include "../libc/stdio.h"
#include "../libc/stdlib.h"
#include "../libc/string.h"
#include "../libc/crypto.h"
#include "../drivers/font8x8_basic.h"

/* === Back-buffer (mapped by kernel at 0x70000000) === */
static uint32_t* fb = 0;
static uint32_t SCR_W = 1024;
static uint32_t SCR_H = 768;
static char logged_in_user[32] = "unknown";

/* === Color Palette === */
#define COL_DESKTOP_TOP    0x0A1628
#define COL_DESKTOP_BOT    0x1A3A5C
#define COL_TASKBAR        0x0D1117
#define COL_TASKBAR_HOVER  0x1C2733
#define COL_START_BTN      0x2EA043
#define COL_WIN_TITLE      0x161B22
#define COL_WIN_TITLE_ACT  0x1F6FEB
#define COL_WIN_BODY       0x0D1117
#define COL_WIN_BORDER     0x30363D
#define COL_CLOSE_BTN      0xDA3633
#define COL_TEXT_PRIMARY    0xE6EDF3
#define COL_TEXT_SECONDARY  0x8B949E
#define COL_TEXT_ACCENT     0x58A6FF
#define COL_ICON_BG        0x161B22
#define COL_MENU_BG        0x161B22
#define COL_MENU_BORDER    0x30363D
#define COL_HIGHLIGHT      0x1F6FEB


/* === Layout === */
#define TASKBAR_H   44
#define CELL_W      16
#define LINE_H      20
#define ICON_SIZE   64
#define ICON_GAP    104
#define TITLE_H     32
#define WIN_BORDER  2
#define START_BTN_W 90
#define MENU_W      220
#define MENU_ITEM_H 36
#define MAX_WINDOWS 4
#define MAX_ICONS   4

/* ================================================================
 * DIRECT BUFFER RENDERING ENGINE (zero syscall overhead)
 * ================================================================ */

/* Fast rectangle fill — clamp once, no per-pixel bounds check */
static void buf_rect(int x, int y, int w, int h, uint32_t c) {
    if (!fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)SCR_W) x1 = (int)SCR_W;
    int y1 = y + h; if (y1 > (int)SCR_H) y1 = (int)SCR_H;
    if (x0 >= x1 || y0 >= y1) return;
    int rw = x1 - x0;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = fb + py * (int)SCR_W + x0;
        for (int i = 0; i < rw; i++) row[i] = c;
    }
}

/* Draw character (scale=2, transparent background) */
static void buf_char(char c, int x, int y, uint32_t fg) {
    if (!fb || (unsigned char)c > 127) return;
    if (x + 16 <= 0 || x >= (int)SCR_W || y + 16 <= 0 || y >= (int)SCR_H) return;
    unsigned char* g = (unsigned char*)font8x8_basic[(int)c];
    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy * 2;
        if (py < 0 || py + 1 >= (int)SCR_H) continue;
        for (int cx = 0; cx < 8; cx++) {
            if (!(g[cy] & (1 << cx))) continue;
            int px = x + cx * 2;
            if (px < 0 || px + 1 >= (int)SCR_W) continue;
            fb[py * SCR_W + px]     = fg;
            fb[py * SCR_W + px + 1] = fg;
            fb[(py+1) * SCR_W + px]     = fg;
            fb[(py+1) * SCR_W + px + 1] = fg;
        }
    }
}

/* Draw text string */
static void buf_text(const char* s, int x, int y, uint32_t fg) {
    for (int i = 0; s[i]; i++)
        buf_char(s[i], x + i * CELL_W, y, fg);
}

/* Hit test */
static int hit(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

/* Int to string */
static void u64_str(uint64_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[20]; int n = 0;
    while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

/* ================================================================
 * WINDOW SYSTEM
 * ================================================================ */

typedef struct {
    int x, y, w, h, visible, dragging, drag_ox, drag_oy, type;
    char title[32];
} window_t;

static window_t windows[MAX_WINDOWS];
static int active_window = -1, window_count = 0;

typedef struct { int x, y, type; char label[16]; char icon; } icon_t;
static icon_t icons[MAX_ICONS];
static int start_menu_open = 0, mouse_prev_btn = 0;

/* Forward declarations */
static void draw_clock(void);
static void draw_window(int idx);
static void redraw_all(void);

/* ================================================================
 * DRAW FUNCTIONS
 * ================================================================ */

/* Cached background buffer — draw gradient once, reuse */
static uint32_t bg_cache[1024]; /* One color per row (max 1024 rows) */
static int bg_cached = 0;

static void draw_background(void) {
    int tb_y = (int)SCR_H - TASKBAR_H;
    if (!bg_cached) {
        for (int y = 0; y < tb_y && y < 1024; y++) {
            int t = (y * 255) / tb_y;
            int r = ((0x0A * (255 - t)) + (0x1A * t)) / 255;
            int g = ((0x16 * (255 - t)) + (0x3A * t)) / 255;
            int b = ((0x28 * (255 - t)) + (0x5C * t)) / 255;
            bg_cache[y] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        bg_cached = 1;
    }
    /* Fill using cached colors — one buf_rect per row */
    for (int y = 0; y < tb_y && y < 1024; y++)
        buf_rect(0, y, (int)SCR_W, 1, bg_cache[y]);
}

static void draw_clock(void) {
    uint64_t ticks = get_ticks();
    uint64_t secs = ticks / 1000, mins = secs / 60, hrs = mins / 60;
    secs %= 60; mins %= 60; hrs %= 24;
    char clock[9];
    clock[0]='0'+(hrs/10); clock[1]='0'+(hrs%10); clock[2]=':';
    clock[3]='0'+(mins/10); clock[4]='0'+(mins%10); clock[5]=':';
    clock[6]='0'+(secs/10); clock[7]='0'+(secs%10); clock[8]='\0';
    int cx = (int)SCR_W - 140, ty = (int)SCR_H - TASKBAR_H;
    buf_rect(cx, ty + 6, 130, TASKBAR_H - 12, COL_TASKBAR);
    buf_text(clock, cx + 10, ty + 12, COL_TEXT_ACCENT);
}

static void draw_taskbar(void) {
    int ty = (int)SCR_H - TASKBAR_H;
    buf_rect(0, ty, (int)SCR_W, TASKBAR_H, COL_TASKBAR);
    buf_rect(0, ty, (int)SCR_W, 1, COL_WIN_BORDER);
    buf_rect(4, ty + 6, START_BTN_W, TASKBAR_H - 12, COL_START_BTN);
    buf_text("GenOS", 14, ty + 12, COL_TEXT_PRIMARY);

    int bx = START_BTN_W + 16;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].visible) continue;
        uint32_t bg = (i == active_window) ? COL_HIGHLIGHT : COL_TASKBAR_HOVER;
        buf_rect(bx, ty + 6, 120, TASKBAR_H - 12, bg);
        char st[8]; int j;
        for (j = 0; j < 7 && windows[i].title[j]; j++) st[j] = windows[i].title[j];
        st[j] = '\0';
        buf_text(st, bx + 8, ty + 12, COL_TEXT_PRIMARY);
        bx += 128;
    }
    /* Show logged-in user on right side of taskbar */
    int ux = (int)SCR_W - 200;
    buf_text(logged_in_user, ux, ty + 12, 0x8B949E);
    draw_clock();
}

static void init_icons(void) {
    const char* labels[] = {"About", "Files", "Term", "System"};
    const char chars[] = {'i', 'F', '>', 'S'};
    for (int i = 0; i < MAX_ICONS; i++) {
        icons[i].x = 30;
        icons[i].y = 30 + i * ICON_GAP;
        icons[i].icon = chars[i];
        icons[i].type = i;
        int j; for (j = 0; labels[i][j]; j++) icons[i].label[j] = labels[i][j];
        icons[i].label[j] = '\0';
    }
}

static void draw_icons(void) {
    for (int i = 0; i < MAX_ICONS; i++) {
        int ix = icons[i].x, iy = icons[i].y;
        buf_rect(ix, iy, ICON_SIZE, ICON_SIZE, COL_ICON_BG);
        buf_rect(ix, iy, ICON_SIZE, 2, COL_WIN_BORDER);
        buf_rect(ix, iy + ICON_SIZE - 2, ICON_SIZE, 2, COL_WIN_BORDER);
        buf_rect(ix, iy, 2, ICON_SIZE, COL_WIN_BORDER);
        buf_rect(ix + ICON_SIZE - 2, iy, 2, ICON_SIZE, COL_WIN_BORDER);
        buf_char(icons[i].icon, ix + 24, iy + 20, COL_TEXT_ACCENT);
        int lx = ix + (ICON_SIZE - strlen(icons[i].label) * CELL_W) / 2;
        buf_text(icons[i].label, lx < ix ? ix : lx, iy + ICON_SIZE + 4, COL_TEXT_PRIMARY);
    }
}

static int create_window(const char* title, int type) {
    /* Terminal: launch shell.elf directly */
    if (type == 2) {
        /* Clear screen and hand off to shell */
        clear_screen();
        flush_screen();
        int pid = exec("shell.elf");
        if (pid > 0) {
            /* Wait for shell to finish */
            while (!wait_pid(pid)) {
                user_sleep(50);
            }
        }
        /* Shell exited — redraw desktop */
        redraw_all();
        return -1;
    }

    for (int i = 0; i < window_count; i++)
        if (windows[i].visible && windows[i].type == type) { active_window = i; return i; }
    if (window_count >= MAX_WINDOWS) return -1;
    window_t* w = &windows[window_count];
    w->w = 420; w->h = 320;
    w->x = 160 + window_count * 40; w->y = 80 + window_count * 30;
    w->visible = 1; w->dragging = 0; w->type = type;
    int j; for (j = 0; title[j] && j < 30; j++) w->title[j] = title[j]; w->title[j] = '\0';
    active_window = window_count++;
    return active_window;
}

static void draw_window(int idx) {
    window_t* w = &windows[idx];
    if (!w->visible) return;
    int act = (idx == active_window);
    uint32_t tcol = act ? COL_WIN_TITLE_ACT : COL_WIN_TITLE;

    buf_rect(w->x + 4, w->y + 4, w->w, w->h, 0x050A10); /* Shadow */
    buf_rect(w->x - WIN_BORDER, w->y - WIN_BORDER,
             w->w + WIN_BORDER*2, w->h + WIN_BORDER*2,
             act ? COL_HIGHLIGHT : COL_WIN_BORDER);
    buf_rect(w->x, w->y, w->w, TITLE_H, tcol);
    buf_text(w->title, w->x + 12, w->y + 8, COL_TEXT_PRIMARY);
    int cbx = w->x + w->w - 36;
    buf_rect(cbx, w->y + 4, 28, 24, COL_CLOSE_BTN);
    buf_char('X', cbx + 6, w->y + 8, COL_TEXT_PRIMARY);
    buf_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H, COL_WIN_BODY);

    int cx = w->x + 16, cy = w->y + TITLE_H + 16;
    switch (w->type) {
    case 0: /* About */
        buf_text("GenOS v3", cx, cy, COL_TEXT_ACCENT);
        buf_text("64-bit UEFI Operating System", cx, cy+LINE_H, COL_TEXT_PRIMARY);
        buf_text("Built by Mandor", cx, cy+LINE_H*2, COL_TEXT_PRIMARY);
        buf_text("Architecture: x86_64", cx, cy+LINE_H*3, COL_TEXT_SECONDARY);
        buf_text("Shell: Ring 3 (User Space)", cx, cy+LINE_H*4, COL_TEXT_SECONDARY);
        buf_text("DE: GenOS Desktop v1.0", cx, cy+LINE_H*5, COL_TEXT_SECONDARY);
        buf_rect(cx, cy+LINE_H*6+4, w->w-32, 1, COL_WIN_BORDER);
        buf_text("Syscalls: 35 | Scheduler: MLFQ", cx, cy+LINE_H*7, COL_TEXT_SECONDARY);
        buf_text("Features: VFS, SHM, Fork, CoW", cx, cy+LINE_H*8, COL_TEXT_SECONDARY);
        buf_text("Rendering: Direct Back-Buffer", cx, cy+LINE_H*9, 0x3FB950);
        break;
    case 1: { /* Files */
        buf_text("Ramdisk Files:", cx, cy, COL_TEXT_ACCENT);
        int fd = open("/", 0);
        if (fd >= 0) {
            char name[100]; int sz = 0, row = 1;
            while (readdir(fd, name, &sz) && row < 12) {
                char line[120]; int j = 0;
                line[j++]=' '; line[j++]=' ';
                for (int k=0; name[k]&&j<80; k++) line[j++]=name[k];
                line[j++]=' '; line[j++]=' ';
                char num[16]; u64_str((uint64_t)(unsigned)sz, num);
                for (int k=0; num[k]&&j<100; k++) line[j++]=num[k];
                line[j++]='B'; line[j]='\0';
                buf_text(line, cx, cy+LINE_H*row, COL_TEXT_PRIMARY);
                row++; sz = 0;
            }
            close(fd);
        }
        break;
    }
    case 2: /* Terminal — launch shell.elf */
        buf_text("Terminal", cx, cy, COL_TEXT_ACCENT);
        buf_text("Launching shell.elf...", cx, cy+LINE_H*2, COL_TEXT_PRIMARY);
        buf_text("Press ESC in shell to", cx, cy+LINE_H*4, COL_TEXT_SECONDARY);
        buf_text("return to desktop.", cx, cy+LINE_H*5, COL_TEXT_SECONDARY);
        break;
    case 3: { /* System Info */
        buf_text("System Information", cx, cy, COL_TEXT_ACCENT);
        buf_rect(cx, cy+LINE_H-2, w->w-32, 1, COL_WIN_BORDER);
        uint64_t ticks = get_ticks(); char buf[24];
        buf_text("Uptime:", cx, cy+LINE_H*2, COL_TEXT_SECONDARY);
        u64_str(ticks/1000, buf); buf_text(buf, cx+130, cy+LINE_H*2, COL_TEXT_PRIMARY);
        buf_text("seconds", cx+130+(strlen(buf)+1)*CELL_W, cy+LINE_H*2, COL_TEXT_SECONDARY);
        buf_text("Screen:", cx, cy+LINE_H*3, COL_TEXT_SECONDARY);
        u64_str(SCR_W, buf); buf_text(buf, cx+130, cy+LINE_H*3, COL_TEXT_PRIMARY);
        buf_text("x", cx+130+strlen(buf)*CELL_W, cy+LINE_H*3, COL_TEXT_SECONDARY);
        u64_str(SCR_H, buf); buf_text(buf, cx+130+5*CELL_W, cy+LINE_H*3, COL_TEXT_PRIMARY);
        buf_text("Renderer:", cx, cy+LINE_H*4, COL_TEXT_SECONDARY);
        buf_text("Direct Back-Buffer", cx+170, cy+LINE_H*4, 0x3FB950);
        cache_stats_t cs;
        if (cache_get_stats(&cs) == 0) {
            buf_text("Cache Hits:", cx, cy+LINE_H*6, COL_TEXT_SECONDARY);
            u64_str(cs.hits, buf); buf_text(buf, cx+200, cy+LINE_H*6, 0x3FB950);
            buf_text("Misses:", cx, cy+LINE_H*7, COL_TEXT_SECONDARY);
            u64_str(cs.misses, buf); buf_text(buf, cx+200, cy+LINE_H*7, COL_CLOSE_BTN);
        }
        break;
    }
    }
}

static void draw_start_menu(void) {
    if (!start_menu_open) return;
    /* 5 items (header + 4 apps) + separator + 2 power items = 8 rows */
    int total_rows = 8;
    int mx = 4, my = (int)SCR_H - TASKBAR_H - (MENU_ITEM_H*total_rows+8);
    buf_rect(mx-1, my-1, MENU_W+2, MENU_ITEM_H*total_rows+10, COL_MENU_BORDER);
    buf_rect(mx, my, MENU_W, MENU_ITEM_H*total_rows+8, COL_MENU_BG);
    buf_rect(mx, my, MENU_W, MENU_ITEM_H, COL_HIGHLIGHT);
    buf_text("GenOS v3", mx+12, my+10, COL_TEXT_PRIMARY);

    /* App items */
    const char* items[] = {"About GenOS","File Manager","Terminal","System Info"};
    for (int i = 0; i < 4; i++) {
        int iy = my + MENU_ITEM_H*(i+1) + 4;
        buf_text(items[i], mx+16, iy+8, COL_TEXT_PRIMARY);
        if (i < 3) buf_rect(mx+8, iy+MENU_ITEM_H-2, MENU_W-16, 1, COL_WIN_BORDER);
    }

    /* Separator line before power options */
    int sep_y = my + MENU_ITEM_H*5 + 4;
    buf_rect(mx+8, sep_y, MENU_W-16, 2, COL_WIN_BORDER);

    /* Power: Restart */
    int ry = my + MENU_ITEM_H*6 + 4;
    buf_text("Restart", mx+16, ry+8, 0xF0883E);

    /* Power: Shutdown */
    int sy = my + MENU_ITEM_H*7 + 4;
    buf_text("Shutdown", mx+16, sy+8, 0xF85149);
}

static void redraw_all(void) {
    draw_background();
    draw_icons();
    for (int i = 0; i < window_count; i++) draw_window(i);
    draw_taskbar();
    draw_start_menu();
    flush_screen(); /* ONE syscall to push entire frame to screen */
}

/* ================================================================
 * EVENT HANDLING
 * ================================================================ */

static void handle_click(int mx, int my) {
    int ty = (int)SCR_H - TASKBAR_H;
    const char* titles[] = {"About GenOS","File Manager","Terminal","System Info"};

    if (start_menu_open) {
        int total_rows = 8;
        int menu_y = ty - (MENU_ITEM_H*total_rows+8);

        /* Check app items (4 items) */
        for (int i = 0; i < 4; i++) {
            int iy = menu_y + MENU_ITEM_H*(i+1) + 4;
            if (hit(mx, my, 4, iy, MENU_W, MENU_ITEM_H)) {
                create_window(titles[i], i);
                start_menu_open = 0; redraw_all(); return;
            }
        }

        /* Check Restart (row 6) */
        int ry = menu_y + MENU_ITEM_H*6 + 4;
        if (hit(mx, my, 4, ry, MENU_W, MENU_ITEM_H)) {
            start_menu_open = 0;
            /* Show restart message */
            buf_rect(0, 0, (int)SCR_W, (int)SCR_H, 0x0D1117);
            buf_text("Restarting...", (int)SCR_W/2 - 100, (int)SCR_H/2, 0xF0883E);
            flush_screen();
            user_sleep(500);
            power_restart();
            return;
        }

        /* Check Shutdown (row 7) */
        int sy = menu_y + MENU_ITEM_H*7 + 4;
        if (hit(mx, my, 4, sy, MENU_W, MENU_ITEM_H)) {
            start_menu_open = 0;
            /* Show shutdown message */
            buf_rect(0, 0, (int)SCR_W, (int)SCR_H, 0x0D1117);
            buf_text("Shutting down...", (int)SCR_W/2 - 120, (int)SCR_H/2, 0xF85149);
            flush_screen();
            user_sleep(500);
            power_shutdown();
            return;
        }

        start_menu_open = 0; redraw_all(); return;
    }

    if (hit(mx, my, 4, ty+6, START_BTN_W, TASKBAR_H-12)) {
        start_menu_open = !start_menu_open; redraw_all(); return;
    }

    int bx = START_BTN_W + 16;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].visible) continue;
        if (hit(mx, my, bx, ty+6, 120, TASKBAR_H-12)) {
            active_window = i; redraw_all(); return;
        }
        bx += 128;
    }

    for (int i = window_count-1; i >= 0; i--) {
        window_t* w = &windows[i];
        if (!w->visible) continue;
        if (hit(mx, my, w->x+w->w-36, w->y+4, 28, 24)) {
            w->visible = 0; if (active_window==i) active_window=-1;
            redraw_all(); return;
        }
        if (hit(mx, my, w->x, w->y, w->w-40, TITLE_H)) {
            w->dragging=1; w->drag_ox=mx-w->x; w->drag_oy=my-w->y;
            active_window=i; redraw_all(); return;
        }
        if (hit(mx, my, w->x, w->y, w->w, w->h)) {
            active_window=i; redraw_all(); return;
        }
    }

    for (int i = 0; i < MAX_ICONS; i++) {
        if (hit(mx, my, icons[i].x, icons[i].y, ICON_SIZE, ICON_SIZE+20)) {
            create_window(titles[icons[i].type], icons[i].type);
            redraw_all(); return;
        }
    }
}

static void handle_drag(int mx, int my) {
    for (int i = 0; i < window_count; i++) {
        window_t* w = &windows[i];
        if (!w->dragging) continue;
        w->x = mx - w->drag_ox; w->y = my - w->drag_oy;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        if (w->x + w->w > (int)SCR_W) w->x = (int)SCR_W - w->w;
        if (w->y + w->h > (int)SCR_H - TASKBAR_H) w->y = (int)SCR_H - TASKBAR_H - w->h;
        redraw_all();
        return;
    }
}

static void handle_release(void) {
    for (int i = 0; i < window_count; i++) windows[i].dragging = 0;
}

/* ================================================================
 * LOGIN SCREEN
 * ================================================================ */

static void login_screen(void) {
    int cx = (int)SCR_W / 2 - 180;
    int cy = (int)SCR_H / 2 - 120;

    int attempts = 0;
    while (attempts < 3) {
        /* Draw login background */
        for (uint32_t y = 0; y < SCR_H; y++) {
            uint32_t r = 8 + (y * 16 / SCR_H);
            uint32_t g = 20 + (y * 30 / SCR_H);
            uint32_t b = 40 + (y * 50 / SCR_H);
            uint32_t col = (r << 16) | (g << 8) | b;
            for (uint32_t x = 0; x < SCR_W; x++)
                fb[y * SCR_W + x] = col;
        }

        /* Login box background */
        int bw = 360, bh = 240;
        int bx = cx - 20, by = cy - 20;
        /* Box border */
        buf_rect(bx-2, by-2, bw+4, bh+4, 0x1F6FEB);
        /* Box body */
        buf_rect(bx, by, bw, bh, 0x0D1117);

        /* Title */
        buf_text("GenOS v3", cx + 100, cy, 0x58A6FF);
        buf_rect(cx, cy + 24, bw - 40, 1, 0x30363D);

        /* OS subtitle */
        buf_text("Secure Login", cx + 80, cy + 35, 0x8B949E);

        /* Username label + field */
        buf_text("Username:", cx, cy + 70, 0xC9D1D9);
        buf_rect(cx, cy + 90, 300, 28, 0x161B22);
        buf_rect(cx, cy + 90, 300, 28, 0x30363D); /* border */
        buf_rect(cx+1, cy+91, 298, 26, 0x161B22);

        /* Password label + field */
        buf_text("Password:", cx, cy + 130, 0xC9D1D9);
        buf_rect(cx, cy + 150, 300, 28, 0x30363D);
        buf_rect(cx+1, cy+151, 298, 26, 0x161B22);

        if (attempts > 0) {
            buf_text("Invalid credentials!", cx + 40, cy + 195, 0xF85149);
        }

        buf_text("Press ENTER to login", cx + 40, cy + 215, 0x484F58);

        flush_screen();

        /* Read username */
        char username[32]; int ui = 0;
        /* Render cursor in username field */
        buf_rect(cx+8, cy+96, 8, 14, 0x58A6FF);
        flush_screen();

        while (1) {
            char k = read_key();
            if (k == '\n') break;
            if (k == '\b' && ui > 0) {
                ui--;
                buf_rect(cx+8 + ui*CELL_W, cy+96, CELL_W, 14, 0x161B22);
                buf_rect(cx+8 + ui*CELL_W, cy+96, 8, 14, 0x58A6FF);
                flush_screen();
                continue;
            }
            if (k && k != '\b' && ui < 30) {
                username[ui] = k;
                buf_rect(cx+8 + ui*CELL_W, cy+96, CELL_W, 14, 0x161B22);
                buf_char(k, cx+8 + ui*CELL_W, cy+96, 0xC9D1D9);
                ui++;
                buf_rect(cx+8 + ui*CELL_W, cy+96, 8, 14, 0x58A6FF);
                flush_screen();
            }
        }
        username[ui] = '\0';

        /* Clear cursor from username, show in password field */
        buf_rect(cx+8 + ui*CELL_W, cy+96, 8, 14, 0x161B22);
        buf_rect(cx+8, cy+156, 8, 14, 0x58A6FF);
        flush_screen();

        /* Read password (show asterisks) */
        char password[64]; int pi = 0;
        while (1) {
            char k = read_key();
            if (k == '\n') break;
            if (k == '\b' && pi > 0) {
                pi--;
                buf_rect(cx+8 + pi*CELL_W, cy+156, CELL_W, 14, 0x161B22);
                buf_rect(cx+8 + pi*CELL_W, cy+156, 8, 14, 0x58A6FF);
                flush_screen();
                continue;
            }
            if (k && k != '\b' && pi < 62) {
                password[pi++] = k;
                buf_rect(cx+8 + (pi-1)*CELL_W, cy+156, CELL_W, 14, 0x161B22);
                buf_char('*', cx+8 + (pi-1)*CELL_W, cy+156, 0xC9D1D9);
                buf_rect(cx+8 + pi*CELL_W, cy+156, 8, 14, 0x58A6FF);
                flush_screen();
            }
        }
        password[pi] = '\0';

        /* Authenticate */
        int uid = crypto_login(username, password);
        if (uid >= 0) {
            /* Copy username for later display */
            for (int i = 0; username[i] && i < 31; i++)
                logged_in_user[i] = username[i];
            logged_in_user[ui] = '\0';

            /* Show success animation */
            buf_rect(bx, by, bw, bh, 0x0D1117);
            buf_text("Welcome,", cx + 80, cy + 60, 0x3FB950);
            buf_text(logged_in_user, cx + 80, cy + 90, 0x58A6FF);
            buf_text("Loading desktop...", cx + 60, cy + 140, 0x8B949E);
            flush_screen();
            user_sleep(800);
            return;
        }

        attempts++;
    }

    /* 3 failed attempts — still allow in (no lockout on bare metal) */
    for (int i = 0; i < 8; i++) logged_in_user[i] = "unknown"[i];
    logged_in_user[7] = '\0';
}

/* ================================================================
 * ENTRY POINT
 * ================================================================ */

void _start(void) {
    /* Get screen dimensions */
    screen_info_t si;
    if (get_screen_info(&si) == 0) { SCR_W = si.width; SCR_H = si.height; }

    /* Map back-buffer into our address space (the key optimization!) */
    fb = map_framebuffer();
    if (!fb) {
        print_at("ERROR: Cannot map framebuffer!", 50, 100, 0xFF0000);
        exit(1);
    }

    /* === LOGIN SCREEN === */
    login_screen();

    /* === DESKTOP === */
    init_icons();
    redraw_all();

    /* Main event loop */
    mouse_state_t ms;
    uint64_t last_clock = 0;

    /*
     * Virtual cursor position — used for BOTH mouse and keyboard control.
     * On bare metal where PS/2 mouse/touchpad may not work, arrow keys
     * move this cursor instead. Enter key simulates a left click.
     */
    int32_t cursor_x = (int32_t)SCR_W / 2;
    int32_t cursor_y = (int32_t)SCR_H / 2;
    int kbd_cursor_active = 0;  /* Set to 1 when arrow keys are used */

    /* Key codes from keyboard driver (must match keyboard.c) */
    #define KEY_UP    0x80
    #define KEY_DOWN  0x81
    #define KEY_LEFT  0x82
    #define KEY_RIGHT 0x83

    while (1) {
        /* === Mouse input === */
        if (read_mouse(&ms) == 0 && ms.changed) {
            int btn = ms.buttons & 1;
            int clicked = (btn && !mouse_prev_btn);
            int released = (!btn && mouse_prev_btn);

            /* Sync virtual cursor with mouse position */
            cursor_x = ms.x;
            cursor_y = ms.y;
            kbd_cursor_active = 0;  /* Mouse is working, hide kbd cursor */

            if (clicked) handle_click(ms.x, ms.y);
            if (btn && mouse_prev_btn) handle_drag(ms.x, ms.y);
            if (released) handle_release();
            mouse_prev_btn = btn;
        }

        /* === Keyboard input (cursor control fallback) === */
        char key = read_key();

        if (key == 27) break; /* ESC = exit desktop */

        if (key == (char)KEY_UP || key == (char)KEY_DOWN ||
            key == (char)KEY_LEFT || key == (char)KEY_RIGHT) {
            /* Arrow keys: move virtual cursor */
            int step = 10;  /* pixels per keypress */
            kbd_cursor_active = 1;

            if (key == (char)KEY_UP)    cursor_y -= step;
            if (key == (char)KEY_DOWN)  cursor_y += step;
            if (key == (char)KEY_LEFT)  cursor_x -= step;
            if (key == (char)KEY_RIGHT) cursor_x += step;

            /* Clamp to screen */
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_x >= (int32_t)SCR_W) cursor_x = (int32_t)SCR_W - 1;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_y >= (int32_t)SCR_H) cursor_y = (int32_t)SCR_H - 1;

            /* Move the kernel hardware cursor to match */
            set_cursor(cursor_x, cursor_y);
        }

        /* Enter key = simulate left click at virtual cursor position */
        if (key == '\n' && kbd_cursor_active) {
            handle_click(cursor_x, cursor_y);
        }

        /* Space key = also simulate click (easier on some keyboards) */
        if (key == ' ' && kbd_cursor_active) {
            handle_click(cursor_x, cursor_y);
        }

        /* Update clock every second */
        uint64_t now = get_ticks();
        if (now - last_clock >= 1000) {
            last_clock = now;
            draw_clock();
            /* Refresh live windows */
            for (int i = 0; i < window_count; i++)
                if (windows[i].visible && windows[i].type == 3) draw_window(i);
            flush_screen();
        }
    }

    clear_screen();
    exit(0);
}

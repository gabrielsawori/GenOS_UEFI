#pragma once
#include <stdint.h>

/*
 * GenOS User-Space Standard I/O Library
 * Semua fungsi berkomunikasi dengan kernel melalui syscall.
 */

/* Cetak teks ke layar pada posisi Y default (syscall 1, kompatibilitas) */
void print(const char* text, int y_pos);

/* Baca 1 karakter dari keyboard. Non-blocking, return 0 jika kosong (syscall 3) */
char read_key(void);

/* Bersihkan seluruh layar (syscall 5) */
void clear_screen(void);

/* Cetak teks pada posisi (x, y) dengan warna foreground (syscall 6) */
void print_at(const char* text, int x, int y, uint32_t color);

/* Gambar 1 karakter pada posisi (x, y) dengan warna foreground (syscall 7) */
void draw_char(char c, int x, int y, uint32_t color);

/*
 * Isi area persegi (x,y,w,h) dengan satu warna (syscall 10).
 * Berguna untuk membersihkan baris terminal sebelum menulis ulang
 * agar teks lama tidak tumpang tindih dengan teks baru.
 */
void fill_rect(int x, int y, int w, int h, uint32_t color);

/* === VFS File Operations (syscalls 12-16) === */

/* Seek modes */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Buka file atau direktori melalui VFS; return FD (>=0) atau -1 */
int open(const char* filename, int flags);

/* Baca hingga count byte dari FD ke buf; return bytes read / -1 */
int read(int fd, void* buf, int count);

/* Tutup file descriptor; return 0 sukses / -1 error */
int close(int fd);

/* Ubah posisi baca file; return posisi baru / -1 */
int seek(int fd, int offset, int whence);

/* Baca entry direktori berikutnya; return 1=found, 0=done */
int readdir(int fd, char* name_buf, int* size_buf);

/* === Write Operations (syscalls 23-25) === */

/* Write up to count bytes to fd; return bytes written or -1 */
int write(int fd, const void* buf, int count);

/* Create a new file in tmpfs; return 0 on success, -1 on error */
int create(const char* filename);

/* Delete a file from tmpfs; return 0 on success, -1 on error */
int unlink(const char* filename);

/* === Mouse Input (syscall 30) === */

/*
 * State mouse (mirror dari drivers/mouse.h kernel, HANYA struct-nya).
 * HARUS identik layout-nya dengan kernel side agar syscall copy benar.
 *  buttons: bit0=kiri, bit1=kanan, bit2=tengah
 *  changed: 1 jika ada event baru sejak read_mouse() terakhir (di-reset kernel)
 */
typedef struct {
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;
    uint8_t  changed;
} mouse_state_t;

/*
 * Ambil snapshot state mouse. Return 0 sukses / -1 jika pointer NULL.
 * Setelah panggilan, flag 'changed' di-kernel di-reset (event dikonsumsi).
 */
int read_mouse(mouse_state_t* m);

/* === Screen Information (syscall 31) === */

typedef struct {
    uint32_t width;
    uint32_t height;
} screen_info_t;

/* Ambil dimensi layar (width, height). Return 0 sukses / -1 error */
int get_screen_info(screen_info_t* info);

/* === Pixel Drawing (syscall 32) === */

/* Gambar 1 piksel pada posisi (x, y) dengan warna tertentu */
void draw_pixel(int x, int y, uint32_t color);

/* === Framebuffer Back-Buffer (syscalls 34-35) === */

/* Map a back-buffer into user space for direct pixel writes. Return: pointer or NULL */
uint32_t* map_framebuffer(void);

/* Flush (copy) the back-buffer to the real screen. Call once per frame. */
void flush_screen(void);

/* === Mouse Diagnostics (syscall 36) === */

/*
 * Diagnostic counters untuk debugging mouse di bare metal.
 * Lihat dokumentasi lengkap di drivers/mouse.h (kernel side).
 *
 *   irq_bytes  = jumlah byte diterima IRQ12 (0 = IRQ12 tidak pernah fire)
 *   packets    = jumlah paket 3-byte lengkap yang diproses
 *   sync_drops = byte dibuang karena bukan byte-awal valid
 */
typedef struct {
    uint32_t irq_bytes;
    uint32_t packets;
    uint32_t sync_drops;
} mouse_stats_t;

/* Ambil snapshot counter diagnostic mouse. Return 0 sukses / -1 error */
int mouse_stats(mouse_stats_t* out);
#pragma once
#include <stdint.h>

/*
 * GenOS PS/2 Mouse Driver
 *
 * Driver ini mengelola mouse PS/2 klasik via port 0x60/0x64 + IRQ12.
 * Setiap gerakan mouse menghasilkan paket 3-byte yang diproses oleh
 * state machine mouse_handler() menjadi posisi absolut (x,y) + tombol.
 *
 * Posisi absolut di-clamp ke resolusi framebuffer (di-set via mouse_reset).
 */

/* State mouse lengkap (dibaca user-space via syscall read_mouse) */
typedef struct {
    int32_t  x;        /* Posisi absolut horizontal (pixel) */
    int32_t  y;        /* Posisi absolut vertikal (pixel) */
    uint8_t  buttons;  /* bit0=kiri, bit1=kanan, bit2=tengah */
    uint8_t  changed;  /* 1 = ada event baru sejak read_mouse terakhir */
} mouse_state_t;

/* Inisialisasi hardware PS/2 aux + unmask IRQ12 (dipanggil sekali saat boot) */
void mouse_init(void);

/*
 * Diagnostic counters snapshot (kernel side).
 * Layout HARUS identik dengan mouse_stats_t di libc/stdio.h (user-space).
 * Keduanya didefinisikan terpisah karena kernel dan user-space tidak
 * berbagi header.
 */
typedef struct {
    uint32_t irq_bytes;
    uint32_t packets;
    uint32_t sync_drops;
} mouse_stats_t;

/*
 * mouse_handler() - Proses 1 byte data mouse dari port 0x60.
 * Dipanggil dari ISR IRQ12 (INT 44) untuk setiap byte yang masuk.
 * State machine mengakumulasi 3 byte menjadi 1 event mouse lengkap.
 */
void mouse_handler(uint8_t data);

/* Ambil pointer ke state mouse global (untuk syscall read_mouse) */
mouse_state_t* mouse_get_state(void);

/*
 * mouse_reset() - Reset posisi mouse & set batas layar.
 * @param width, height: resolusi framebuffer untuk clamp posisi.
 * Dipanggil setelah fb_init() agar mouse tahu batas layar.
 */
void mouse_reset(uint32_t width, uint32_t height);

/*
 * === DIAGNOSTIC COUNTERS (bare-metal debugging) ===
 *
 * Saat mouse/touchpad tidak responsif di bare metal, counter ini
 * membedakan tiga skenario tanpa perlu serial log:
 *
 *   irq_count   = jumlah byte yang diterima IRQ12 dari PS/2 controller
 *   pkt_count   = jumlah paket 3-byte lengkap yang berhasil diproses
 *   sync_drops  = jumlah byte yang dibuang karena bukan byte-awal valid
 *
 * Interpretasi:
 *   irq_count == 0        → IRQ12 tidak pernah fire. Kemungkinan:
 *                             - BIOS USB Legacy Support OFF (mouse USB)
 *                             - Touchpad pakai I2C-HID, bukan PS/2
 *                             - IRQ12 ter-mask atau tidak ter-route
 *   irq_count > 0, pkt_count == 0 → IRQ sampai tapi state machine
 *                             tidak sinkron (sync_drops besar)
 *   pkt_count > 0         → mouse berfungsi, masalah ada di UI desktop
 */
uint32_t mouse_get_irq_count(void);
uint32_t mouse_get_pkt_count(void);
uint32_t mouse_get_sync_drops(void);

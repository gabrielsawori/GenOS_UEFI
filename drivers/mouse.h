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

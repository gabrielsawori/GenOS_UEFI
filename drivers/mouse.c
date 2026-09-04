/*
 * GenOS PS/2 Mouse Driver
 *
 * Implementasi driver mouse PS/2 klasik. Berkomunikasi dengan PS/2 controller
 * via port 0x64 (status/command) dan 0x60 (data). Mouse memakai IRQ12 yang
 * di-routing ke INT 44 setelah PIC remap.
 *
 * === URUTAN INIT PS/2 AUX (mengikuti standar OSDev) ===
 *   1. Tunggu input buffer kosong
 *   2. Cmd 0xA8  — enable auxiliary device (mouse port)
 *   3. Cmd 0x20  — baca Controller Configuration Byte (CCB)
 *   4. Set bit 1 (aux interrupt enable) + bit 5 (clock aux) di CCB
 *   5. Cmd 0x60  — tulis CCB kembali (TIDAK mengubah bit keyboard)
 *   6. Cmd 0xD6  — kirim 0xF4 (enable streaming) ke aux device via 0xD4
 *   7. pic_unmask_irq(2)  — enable cascade master→slave
 *   8. pic_unmask_irq(12) — enable mouse IRQ
 *
 * Catatan: kita TIDAK men-disable keyboard selama sequence ini, karena
 * PIC sudah di-remap dan interrupt masih cli (mouse_init dipanggil
 * sebelum timer_init/sti di kernel.c). Aman dari race.
 *
 * === PROTOKOL PAKET PS/2 MOUSE (3 byte) ===
 *   Byte 0 (flags):
 *     bit 0 = tombol kiri
 *     bit 1 = tombol kanan
 *     bit 2 = tombol tengah
 *     bit 3 = selalu 1 (reserved)
 *     bit 4 = sign bit delta X
 *     bit 5 = sign bit delta Y
 *     bit 6 = overflow X
 *     bit 7 = overflow Y
 *   Byte 1: delta X (9-bit signed, bit 4 byte0 = sign)
 *   Byte 2: delta Y (9-bit signed, bit 5 byte0 = sign)
 *
 * Catatan: sumbu Y mouse PS/2 positif ke BAWAH biasanya dibalik, tapi
 * karena framebuffer Y juga positif ke bawah, kita pakai delta apa adanya.
 */
#include "mouse.h"
#include "io.h"
#include "../cpu/pic.h"
#include "serial.h"

/* === STATUS REGISTER (port 0x64) bit definitions === */
#define STATUS_OUT_BUF_FULL  0x01   /* Output buffer penuh (data siap dibaca) */
#define STATUS_IN_BUF_FULL   0x02   /* Input buffer penuh (tunggu sebelum tulis) */
#define STATUS_AUX_DATA      0x20   /* Data berasal dari aux (mouse) */

/* === PS/2 CONTROLLER COMMANDS === */
#define CMD_READ_CONFIG      0x20   /* Baca Controller Configuration Byte */
#define CMD_WRITE_CONFIG     0x60   /* Tulis Controller Configuration Byte */
#define CMD_ENABLE_AUX       0xA8   /* Enable auxiliary port (mouse) */
#define CMD_WRITE_AUX        0xD4   /* Byte berikutnya dikirim ke aux device */

/* === MOUSE DEVICE COMMANDS (dikirim via CMD_WRITE_AUX) === */
#define MOUSE_CMD_ENABLE     0xF4   /* Enable streaming data reporting */

/* === CCB (Controller Configuration Byte) bit definitions === */
#define CCB_FIRST_PORT_INT   0x01   /* Interrupt keyboard */
#define CCB_SECOND_PORT_INT  0x02   /* Interrupt mouse (aux) */
#define CCB_SYSTEM_FLAG      0x04
#define CCB_FIRST_CLK        0x10   /* Clock keyboard */
#define CCB_SECOND_CLK       0x20   /* Clock mouse (aux) */

/* === State machine posisi byte dalam paket 3-byte === */
#define MOUSE_PACKET_LEN     3
static uint8_t  packet_cycle = 0;        /* 0..2: byte ke berapa yang ditunggu */
static uint8_t  packet[MOUSE_PACKET_LEN]; /* Buffer paket sementara */

/* === Diagnostic counters (lihat dokumentasi di mouse.h) === */
static uint32_t irq_count   = 0;  /* Byte yang diterima IRQ12 */
static uint32_t pkt_count   = 0;  /* Paket 3-byte lengkap */
static uint32_t sync_drops  = 0;  /* Byte yang dibuang (mis-sync) */

/* === Global mouse state (dibaca user-space via syscall) === */
static mouse_state_t mouse_state = {
    .x = 0, .y = 0, .buttons = 0, .changed = 0
};

/* Batas layar untuk clamp posisi absolut */
static uint32_t screen_width  = 1024;
static uint32_t screen_height = 768;

/* ------------------------------------------------------------------ */
/* Helper I/O: tunggu kondisi port PS/2 controller                     */
/* ------------------------------------------------------------------ */

/* Tunggu sampai input buffer kosong (siap menerima command/data) */
static void mouse_wait_write(void) {
    for (int i = 0; i < 500000; i++) {
        if (!(inb(0x64) & STATUS_IN_BUF_FULL)) return;
        io_wait();
    }
}

/* Tunggu sampai output buffer penuh (data siap dibaca). */
static void mouse_wait_read(void) {
    for (int i = 0; i < 500000; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) return;
        io_wait();
    }
}

/* Wait for mouse ACK (0xFA) with timeout. Returns 1 on ACK, 0 on timeout. */
static int mouse_wait_ack(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) {
            uint8_t resp = inb(0x60);
            if (resp == 0xFA) return 1;  /* ACK */
            if (resp == 0xFE) return 0;  /* NACK / resend */
        }
        io_wait();
    }
    return 0;
}

/* Send command to mouse and wait for ACK. Returns 1 on success. */
static int mouse_cmd_ack(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, CMD_WRITE_AUX);
    mouse_wait_write();
    outb(0x60, cmd);
    return mouse_wait_ack();
}

void mouse_init(void) {
    serial_write_string("[INFO] Menginisialisasi mouse PS/2...\n");

    /* 0. Disable keyboard and aux during initialization */
    mouse_wait_write(); outb(0x64, 0xAD); /* Disable keyboard */
    mouse_wait_write(); outb(0x64, 0xA7); /* Disable aux */

    /* Flush output buffer */
    for (int i = 0; i < 100; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) inb(0x60);
        io_wait();
    }

    /* 1. PS/2 Controller self-test (command 0xAA) */
    mouse_wait_write();
    outb(0x64, 0xAA);
    mouse_wait_read();
    uint8_t selftest = inb(0x60);
    if (selftest == 0x55) {
        serial_write_string("  [PS2] Controller self-test: PASS\n");
    } else {
        serial_write_string("  [PS2] Controller self-test: FAIL (0x");
        /* Print hex byte */
        char hex[3];
        hex[0] = "0123456789ABCDEF"[(selftest >> 4) & 0xF];
        hex[1] = "0123456789ABCDEF"[selftest & 0xF];
        hex[2] = 0;
        serial_write_string(hex);
        serial_write_string(") — PS/2 controller may not exist\n");
    }

    /* 2. Enable auxiliary port */
    mouse_wait_write();
    outb(0x64, CMD_ENABLE_AUX);

    /* 3. Test auxiliary port (command 0xA9) */
    mouse_wait_write();
    outb(0x64, 0xA9);
    mouse_wait_read();
    uint8_t auxtest = inb(0x60);
    if (auxtest == 0x00) {
        serial_write_string("  [PS2] Aux port test: PASS\n");
    } else {
        serial_write_string("  [PS2] Aux port test: FAIL — no PS/2 mouse port\n");
        serial_write_string("  [PS2] Mouse will not work. Use arrow keys + Enter.\n");
    }

    /* 4. Re-enable aux port (self-test may have disabled it) */
    mouse_wait_write();
    outb(0x64, CMD_ENABLE_AUX);

    /* 5. Read and configure Controller Configuration Byte */
    mouse_wait_write();
    outb(0x64, CMD_READ_CONFIG);
    mouse_wait_read();
    uint8_t config = inb(0x60);

    config |= CCB_SECOND_PORT_INT;   /* Enable IRQ12 */
    config |= CCB_FIRST_PORT_INT;    /* Keep IRQ1 (keyboard) enabled */
    config &= ~CCB_SECOND_CLK;       /* Enable aux clock (clear = enable) */

    mouse_wait_write();
    outb(0x64, CMD_WRITE_CONFIG);
    mouse_wait_write();
    outb(0x60, config);

    /* 6. Re-enable keyboard */
    mouse_wait_write(); outb(0x64, 0xAE);

    /* 7. Reset mouse device (0xFF) — wait for self-test response */
    if (mouse_cmd_ack(0xFF)) {
        serial_write_string("  [PS2] Mouse reset ACK received\n");
        /* Wait for self-test pass (0xAA) and mouse ID (0x00) */
        for (int i = 0; i < 500000; i++) {
            if (inb(0x64) & STATUS_OUT_BUF_FULL) {
                uint8_t b = inb(0x60);
                if (b == 0xAA) {
                    serial_write_string("  [PS2] Mouse self-test: PASS\n");
                    /* Read mouse ID byte */
                    mouse_wait_read();
                    inb(0x60); /* Consume ID byte (usually 0x00) */
                    break;
                }
            }
            io_wait();
        }
    } else {
        serial_write_string("  [PS2] Mouse reset: no ACK (device may not exist)\n");
        /* Flush any leftover bytes */
        for (int i = 0; i < 50; i++) {
            if (inb(0x64) & STATUS_OUT_BUF_FULL) inb(0x60);
            io_wait();
        }
    }

    /* 8. Set default parameters */
    mouse_cmd_ack(0xF6); /* Set defaults */

    /* 9. Set sample rate 100 */
    mouse_cmd_ack(0xF3);
    mouse_cmd_ack(100);

    /* 10. Enable data streaming (0xF4) */
    if (mouse_cmd_ack(MOUSE_CMD_ENABLE)) {
        serial_write_string("  [PS2] Mouse streaming enabled\n");
    } else {
        serial_write_string("  [PS2] Mouse streaming: no ACK\n");
    }

    /* 11. Flush leftover bytes */
    for (int i = 0; i < 50; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) inb(0x60);
        io_wait();
    }

    /* 12. Reset state machine */
    packet_cycle = 0;
    mouse_state.x = (int32_t)(screen_width / 2);
    mouse_state.y = (int32_t)(screen_height / 2);
    mouse_state.buttons = 0;
    mouse_state.changed = 0;

    /* 13. Enable interrupts: cascade (IRQ2) + mouse (IRQ12) */
    pic_unmask_irq(2);
    pic_unmask_irq(12);

    serial_write_string("[OK] Mouse PS/2 siap! (IRQ12 di-unmask)\n");
    serial_write_string("     Fallback: Arrow keys + Enter for cursor control\n");
}

void mouse_reset(uint32_t width, uint32_t height) {
    screen_width  = width  ? width  : 1024;
    screen_height = height ? height : 768;
    /* Posisikan kursor di tengah layar */
    mouse_state.x = (int32_t)(screen_width / 2);
    mouse_state.y = (int32_t)(screen_height / 2);
    mouse_state.changed = 1;
}

mouse_state_t* mouse_get_state(void) {
    return &mouse_state;
}

/* ------------------------------------------------------------------ */
/* Handler IRQ12 — state machine paket 3-byte                          */
/* ------------------------------------------------------------------ */

void mouse_handler(uint8_t data) {
    irq_count++;  /* Diagnostic: setiap byte dari IRQ12 dihitung */

    /*
     * Sinkronisasi: byte pertama paket selalu punya bit 3 = 1 (reserved/always 1).
     * Jika kita belum di byte 0 tapi data ini bukan byte-awal valid, reset state
     * machine untuk hindari misalignment (mis. setelah IRQ hilang).
     */
    if (packet_cycle == 0 && !(data & 0x08)) {
        sync_drops++;  /* Diagnostic: byte dibuang karena mis-sync */
        return; /* Bukan byte awal yang valid, buang */
    }

    packet[packet_cycle] = data;
    packet_cycle++;

    if (packet_cycle < MOUSE_PACKET_LEN) {
        return; /* Belum lengkap, tunggu byte berikutnya */
    }

    /* Paket lengkap (3 byte). Reset cycle untuk paket berikutnya. */
    packet_cycle = 0;
    pkt_count++;  /* Diagnostic: paket lengkap berhasil diproses */

    /* DEBUG: Log setiap 10 paket agar serial tidak flooding */
    if (pkt_count % 10 == 0) {
        serial_write_string("[DEBUG] Mouse: 10 packets received\n");
    }

    uint8_t flags  = packet[0];


    int16_t dx     = (int16_t)(int8_t)packet[1];  /* sign-extend delta X */
    int16_t dy     = (int16_t)(int8_t)packet[2];  /* sign-extend delta Y */

    /* Apply delta ke posisi absolut, clamp ke [0, width/height].
     *
     * BUG FIX (Bare Metal): PS/2 mouse spec: delta Y positif = ke ATAS.
     * Tapi framebuffer Y positif = ke BAWAH. Harus negate dy.
     * QEMU kadang meng-invert Y secara internal sehingga bug ini
     * tidak terlihat di emulator, tapi bare metal/touchpad mengikuti
     * spesifikasi PS/2 secara ketat.
     */
    int32_t nx = mouse_state.x + dx;
    int32_t ny = mouse_state.y - dy;  /* NEGATE: PS/2 Y↑ → screen Y↓ */

    if (nx < 0) nx = 0;
    if (nx >= (int32_t)screen_width)  nx = (int32_t)screen_width - 1;
    if (ny < 0) ny = 0;
    if (ny >= (int32_t)screen_height) ny = (int32_t)screen_height - 1;

    mouse_state.x = nx;
    mouse_state.y = ny;
    mouse_state.buttons = flags & 0x07;  /* Hanya 3 bit tombol */
    mouse_state.changed = 1;
}

/* === Diagnostic getters (lihat dokumentasi di mouse.h) === */
uint32_t mouse_get_irq_count(void)  { return irq_count; }
uint32_t mouse_get_pkt_count(void)  { return pkt_count; }
uint32_t mouse_get_sync_drops(void) { return sync_drops; }

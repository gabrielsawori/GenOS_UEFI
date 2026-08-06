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
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & STATUS_IN_BUF_FULL)) return;
        io_wait();
    }
}

/* Tunggu sampai output buffer penuh (data siap dibaca) */
static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) return;
        io_wait();
    }
}

/* Kirim 1 byte ke mouse device (aux) melalui cmd 0xD4 */
static void mouse_send_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, CMD_WRITE_AUX);
    mouse_wait_write();
    outb(0x60, cmd);
    /* Konsumsi ACK (0xFA) dari mouse */
    mouse_wait_read();
    inb(0x60);
}

/* ------------------------------------------------------------------ */
/* Inisialisasi                                                        */
/* ------------------------------------------------------------------ */

void mouse_init(void) {
    serial_write_string("[INFO] Menginisialisasi mouse PS/2...\n");

    /* 1. Enable auxiliary port (mouse port pada controller) */
    mouse_wait_write();
    outb(0x64, CMD_ENABLE_AUX);
    /* Konsumsi ACK/response */
    mouse_wait_read();
    inb(0x60);

    /* 2. Baca Controller Configuration Byte */
    mouse_wait_write();
    outb(0x64, CMD_READ_CONFIG);
    mouse_wait_read();
    uint8_t config = inb(0x60);

    /* 3. Set bit aux interrupt enable. CLEAR aux clock disable bit.
     *    bit1 (CCB_SECOND_PORT_INT) = 1 → enable IRQ12 mouse
     *    bit5 (CCB_SECOND_CLK)      = 0 → enable clock (0 = enabled!)
     *
     * BUG FIX: CCB bit 5 is "disable aux clock". Setting it DISABLES
     * the mouse! Must be cleared. QEMU ignores this but bare metal
     * strictly follows the spec. */
    config |= CCB_SECOND_PORT_INT;   /* Enable IRQ12 */
    config &= ~CCB_SECOND_CLK;       /* Enable clock (clear = enable) */

    /* 4. Tulis CCB kembali */
    mouse_wait_write();
    outb(0x64, CMD_WRITE_CONFIG);
    mouse_wait_write();
    outb(0x60, config);

    /* 5. Reset mouse device (0xFF) — required on some bare metal.
     * Mouse runs self-test and responds 0xFA (ACK), 0xAA (pass), 0x00 (ID). */
    mouse_send_cmd(0xFF);
    /* Wait for self-test completion bytes (0xAA, 0x00) */
    mouse_wait_read(); inb(0x60); /* 0xAA */
    mouse_wait_read(); inb(0x60); /* 0x00 */

    /* 6. Set sample rate to 100 (standard) */
    mouse_send_cmd(0xF3); /* Set sample rate command */
    mouse_send_cmd(100);  /* 100 samples/sec */

    /* 7. Kirim 0xF4 ke mouse: enable data stream reporting */
    mouse_send_cmd(MOUSE_CMD_ENABLE);

    /* 8. Flush sisa byte yang mungkin tersisa di buffer */
    for (int i = 0; i < 20; i++) {
        if (inb(0x64) & STATUS_OUT_BUF_FULL) {
            inb(0x60);
        }
        io_wait();
    }

    /* 7. Reset state machine paket */
    packet_cycle = 0;
    mouse_state.x = (int32_t)(screen_width / 2);
    mouse_state.y = (int32_t)(screen_height / 2);
    mouse_state.buttons = 0;
    mouse_state.changed = 0;

    /* 8. Enable interrupt: cascade master→slave (IRQ2) + mouse (IRQ12) */
    pic_unmask_irq(2);    /* Master IRQ2 = cascade ke slave PIC */
    pic_unmask_irq(12);   /* Slave IRQ4 (= IRQ12 global) = mouse */

    serial_write_string("[OK] Mouse PS/2 siap! (IRQ12 di-unmask)\n");
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
    /*
     * Sinkronisasi: byte pertama paket selalu punya bit 3 = 1 (reserved/always 1).
     * Jika kita belum di byte 0 tapi data ini bukan byte-awal valid, reset state
     * machine untuk hindari misalignment (mis. setelah IRQ hilang).
     */
    if (packet_cycle == 0 && !(data & 0x08)) {
        return; /* Bukan byte awal yang valid, buang */
    }

    packet[packet_cycle] = data;
    packet_cycle++;

    if (packet_cycle < MOUSE_PACKET_LEN) {
        return; /* Belum lengkap, tunggu byte berikutnya */
    }

    /* Paket lengkap (3 byte). Reset cycle untuk paket berikutnya. */
    packet_cycle = 0;

    uint8_t flags  = packet[0];
    int16_t dx     = (int16_t)(int8_t)packet[1];  /* sign-extend delta X */
    int16_t dy     = (int16_t)(int8_t)packet[2];  /* sign-extend delta Y */

    /* Apply delta ke posisi absolut, clamp ke [0, width/height] */
    int32_t nx = mouse_state.x + dx;
    int32_t ny = mouse_state.y + dy;

    if (nx < 0) nx = 0;
    if (nx >= (int32_t)screen_width)  nx = (int32_t)screen_width - 1;
    if (ny < 0) ny = 0;
    if (ny >= (int32_t)screen_height) ny = (int32_t)screen_height - 1;

    mouse_state.x = nx;
    mouse_state.y = ny;
    mouse_state.buttons = flags & 0x07;  /* Hanya 3 bit tombol */
    mouse_state.changed = 1;
}

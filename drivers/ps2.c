#include "ps2.h"
#include "io.h"
#include "input.h"
#include "keyboard.h"
#include "serial.h"
#include "../cpu/pic.h"

static int mouse_present;
static uint8_t packet[3];
static unsigned packet_used;

static int wait_write(void) {
    for (unsigned i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 2)) return 1;
        asm volatile("pause");
    }
    return 0;
}
static int command(uint8_t value) {
    if (!wait_write()) return 0;
    outb(0x64, value);
    return 1;
}
static int data_write(uint8_t value) {
    if (!wait_write()) return 0;
    outb(0x60, value);
    return 1;
}
static int read_byte(int aux) {
    for (unsigned i = 0; i < 100000; i++) {
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t value = inb(0x60);
            if (status & 0xc0) continue;
            if (!!(status & 0x20) == aux) return value;
            if (!(status & 0x20)) keyboard_handler(value);
        }
        asm volatile("pause");
    }
    return -1;
}
static int mouse_command(uint8_t value) {
    for (unsigned retry = 0; retry < 3; retry++) {
        if (!command(0xd4) || !data_write(value)) return 0;
        int reply = read_byte(1);
        if (reply == 0xfa) return 1;
        if (reply != 0xfe) return 0;
    }
    return 0;
}
void ps2_init(void) {
    mouse_present = 0;
    packet_used = 0;
    if (!command(0xad) || !command(0xa7)) goto unavailable;
    for (unsigned i = 0; i < 256 && (inb(0x64) & 1); i++) (void)inb(0x60);
    if (!command(0x20)) goto unavailable;
    int config = read_byte(0);
    if (config < 0) goto unavailable;
    /* Preserve translated set-1 keyboard mode; enable keyboard port/IRQ. */
    uint8_t settings = (config | 0x41) & ~0x12U;
    if (!command(0x60) || !data_write(settings) || !command(0xae) || !command(0xa8))
        goto unavailable;
    if (!mouse_command(0xf6) || !mouse_command(0xf4)) goto unavailable;
    settings = (settings | 2) & ~0x20U;
    if (!command(0x60) || !data_write(settings)) goto unavailable;
    mouse_present = 1;
    pic_unmask_irq(2);
    pic_unmask_irq(12);
    serial_write_string("[OK] PS/2 keyboard and mouse enabled\n");
    return;
unavailable:
    (void)command(0xae);
    serial_write_string("[WARN] PS/2 mouse unavailable; keyboard retained\n");
}
int ps2_mouse_present(void) { return mouse_present; }

void ps2_mouse_byte(uint8_t data) {
    if (!packet_used && !(data & 8)) return;
    packet[packet_used++] = data;
    if (packet_used != 3) return;
    packet_used = 0;
    if (packet[0] & 0xc0) return; /* Overflow packet, do not invent movement. */
    int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    struct genos_input_event event = {.type = GENOS_INPUT_MOUSE,
        .code = packet[0] & 7, .dx = dx, .dy = -dy};
    input_push(&event);
}

void ps2_irq(void) {
    /* Status chooses the destination: IRQ1/12 may arrive after a byte from
     * the other port. Bound draining so a device cannot monopolize the CPU. */
    for (unsigned i = 0; i < 32; i++) {
        uint8_t status = inb(0x64);
        if (!(status & 1)) break;
        uint8_t value = inb(0x60);
        if (status & 0xc0) { packet_used = 0; continue; }
        if (status & 0x20) { if (mouse_present) ps2_mouse_byte(value); }
        else keyboard_handler(value);
    }
}

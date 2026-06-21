#pragma once
#include <stdint.h>

// Fungsi untuk mengirim data 1 byte ke perangkat keras
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

// Fungsi untuk menerima data 1 byte dari perangkat keras
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

/* I/O port delay (~1-2μs on real hardware).
 * Reading from port 0x80 (POST diagnostic port) is a harmless
 * way to waste time. Required by 8259A PIC during initialization
 * on bare metal (QEMU doesn't need this, but it's harmless there).
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}
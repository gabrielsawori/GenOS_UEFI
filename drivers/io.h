#pragma once
#include <stdint.h>

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
static inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

// Fungsi untuk mengirim data 1 byte ke perangkat keras
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) : "memory" );
}

// Fungsi untuk menerima data 1 byte dari perangkat keras
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory" );
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

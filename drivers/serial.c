#include "serial.h"
#include "io.h"

#define COM1_PORT 0x3f8 // Alamat standar untuk Serial Port COM1

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    // Matikan semua interupsi serial
    outb(COM1_PORT + 3, 0x80);    // Aktifkan mode pengaturan baud rate
    outb(COM1_PORT + 0, 0x03);    // Set baud rate ke 38400 (Low byte)
    outb(COM1_PORT + 1, 0x00);    // Set baud rate (High byte)
    outb(COM1_PORT + 3, 0x03);    // Format: 8 bit data, tanpa parity, 1 stop bit
    outb(COM1_PORT + 2, 0xC7);    // Aktifkan FIFO, bersihkan buffer
    outb(COM1_PORT + 4, 0x0B);    // Siapkan port untuk transmisi
}

// Mengecek apakah antrean pengiriman sudah kosong
int is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

// ===== FIX BUG #2: Tambahkan timeout untuk mencegah infinite loop =====
void serial_write_char(char c) {
    uint32_t timeout = 10000;
    
    // Tunggu sampai jalur kosong atau timeout
    while ((is_transmit_empty() == 0) && (timeout-- > 0)) {
        asm volatile ("nop"); // Hindari compiler optimization
    }
    
    // Jika masih ada waktu, kirim karakter
    if (timeout > 0) {
        outb(COM1_PORT, c);
    }
}

void serial_write_string(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        serial_write_char(str[i]);
    }
}

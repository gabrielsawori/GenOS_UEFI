#include "timer.h"
#include "io.h"
#include "serial.h"

// Variabel global untuk menghitung berapa kali jantung OS berdetak
static volatile uint64_t ticks = 0;

void timer_init(uint32_t frequency) {
    // Clock dasar dari chip PIT adalah 1193180 Hz
    uint32_t divisor = 1193180 / frequency; 
    
    // Kirim perintah "Command Byte" ke port 0x43 (meminta izin mengatur PIT)
    outb(0x43, 0x36);
    
    // Kirim angka divisor ke port 0x40 (Byte bawah dulu, lalu Byte atas)
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    
    serial_write_string("[OK] Programmable Interval Timer (PIT) diinisialisasi.\n");
}

// Fungsi ini akan dipanggil oleh CPU setiap kali PIT berdetak
void timer_tick(void) {
    ticks++;
}

// Fungsi untuk melihat sudah berapa milidetik OS menyala
uint64_t timer_get_ticks(void) {
    return ticks;
}

// Fungsi ajaib untuk menunda waktu (Delay/Sleep) dalam satuan milidetik
void sleep(uint32_t ms) {
    uint64_t start_time = ticks;
    // Tahan CPU di sini sampai jumlah detak mencapai target
    while (ticks < start_time + ms) {
        __asm__ volatile ("hlt"); // Hlt agar CPU hemat daya saat menunggu
    }
}
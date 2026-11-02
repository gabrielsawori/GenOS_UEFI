#include "timer.h"
#include "io.h"
#include "serial.h"

// Variabel global untuk menghitung berapa kali jantung OS berdetak
static uint64_t ticks = 0;

void timer_init(uint32_t frequency) {
    if (!frequency || frequency > 1193180 || 1193180 / frequency > 65536) {
        serial_write_string("[ERROR] Invalid PIT frequency\n");
        return;
    }
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
    __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
}

// Fungsi untuk melihat sudah berapa milidetik OS menyala
uint64_t timer_get_ticks(void) {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

// Fungsi ajaib untuk menunda waktu (Delay/Sleep) dalam satuan milidetik
void sleep(uint32_t ms) {
    uint64_t start_time = timer_get_ticks();
    // Tahan CPU di sini sampai jumlah detak mencapai target
    while (timer_get_ticks() - start_time < ms) {
        __asm__ volatile ("hlt"); // Hlt agar CPU hemat daya saat menunggu
    }
}

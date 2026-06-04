/*
 * GenOS Interactive Demo Application (Ring 3)
 *
 * Aplikasi ini mendemonstrasikan semua syscall yang tersedia:
 *   - print()      : mencetak teks ke layar (syscall 1)
 *   - read_key()   : membaca input keyboard (syscall 3)
 *   - user_sleep() : menunggu N milidetik (syscall 4)
 *   - exit()       : mengakhiri aplikasi (syscall 2)
 *
 * Alur program:
 *   1. Tampilkan pesan selamat datang
 *   2. Minta user menekan sembarang tombol
 *   3. Tampilkan tombol yang ditekan
 *   4. Hitung mundur 3 detik
 *   5. Keluar dengan aman
 */
#include "../libc/stdio.h"
#include "../libc/stdlib.h"

void _start(void) {
    /* Tahap 1: Pesan selamat datang */
    print("=== GENOS INTERACTIVE APP (Ring 3) ===", 300);
    print("Syscall aktif: print, read_key, sleep, exit", 324);
    print("Tekan sembarang tombol...", 360);

    /* Tahap 2: Tunggu input keyboard (polling dengan sleep) */
    char c = 0;
    while (c == 0) {
        c = read_key();
        if (c == 0) {
            user_sleep(10); /* Tidur 10ms agar tidak boros CPU */
        }
    }

    /* Tahap 3: Tampilkan tombol yang ditekan */
    char msg[] = "Kamu menekan tombol: [ ]";
    msg[21] = c; /* Sisipkan karakter ke dalam string */
    print(msg, 384);

    /* Tahap 4: Hitung mundur sebelum keluar */
    print("Keluar dalam 3...", 420);
    user_sleep(1000);
    print("Keluar dalam 2...", 444);
    user_sleep(1000);
    print("Keluar dalam 1...", 468);
    user_sleep(1000);
    print("Selamat tinggal dari Ring 3!", 504);

    /* Tahap 5: Keluar dengan aman */
    exit(0);
}
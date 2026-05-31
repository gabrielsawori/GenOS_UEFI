#include "../libc/stdio.h"
#include "../libc/stdlib.h" // Masukkan perpustakaan standar yang baru

void _start(void) {
    print("Hello from GenOS C Library!", 350);
    print("This is a REAL 'C' application running in Ring 3.", 380);
    print("My task here is done. Exiting gracefully...", 410);
    
    // Ucapkan selamat tinggal!
    exit(0);
}
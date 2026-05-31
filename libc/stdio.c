#include "stdio.h"
#include "syscall.h"

// Aplikasi cukup memanggil ini, dan libc akan mengurus komunikasinya dengan Kernel!
void print(const char* text, int y_pos) {
    // 1 adalah kode Syscall kita untuk mencetak ke layar
    syscall(1, (uint64_t)text, (uint64_t)y_pos, 0);
}
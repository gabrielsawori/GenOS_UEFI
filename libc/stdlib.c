#include "stdlib.h"
#include "syscall.h"

void exit(int status) {
    // Panggil Syscall Nomor 2 (arg1 adalah kode status keluar)
    syscall(2, (uint64_t)status, 0, 0);
    
    // Cegah CPU mengeksekusi instruksi lebih lanjut
    while(1);
}
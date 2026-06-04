#include "stdlib.h"
#include "syscall.h"

/*
 * exit() - Mengakhiri aplikasi Ring-3 yang sedang berjalan.
 *
 * Memanggil syscall nomor 2 (SYS_EXIT).
 * Kernel akan menandai task sebagai TASK_DEAD dan scheduler
 * akan melewatinya pada iterasi berikutnya.
 *
 * Fungsi ini TIDAK PERNAH kembali ke caller.
 *
 * @param status: kode status keluar (0 = sukses, lainnya = error)
 */
void exit(int status) {
    syscall(2, (uint64_t)status, 0, 0);
    /* Cegah CPU melanjutkan eksekusi jika syscall entah kenapa kembali */
    while(1);
}

/*
 * user_sleep() - Menahan eksekusi selama N milidetik.
 *
 * Memanggil syscall nomor 4 (SYS_SLEEP).
 * Timer PIT berdetak 1000 Hz, jadi:
 *   user_sleep(100)  = tidur 0.1 detik
 *   user_sleep(1000) = tidur 1 detik
 *   user_sleep(5000) = tidur 5 detik
 *
 * Selama tidur, scheduler tetap dapat context-switch ke task lain.
 *
 * @param milliseconds: durasi tidur dalam milidetik
 */
void user_sleep(uint32_t milliseconds) {
    syscall(4, (uint64_t)milliseconds, 0, 0);
}
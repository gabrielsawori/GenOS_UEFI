#include "stdio.h"
#include "syscall.h"

/*
 * print() - Mencetak teks ke layar dari aplikasi Ring-3.
 *
 * Memanggil syscall nomor 1 (SYS_PRINT).
 * Kernel akan menampilkan teks di framebuffer dan serial output.
 *
 * @param text:  pointer ke string yang akan dicetak
 * @param y_pos: koordinat Y pada layar (0 = posisi default)
 */
void print(const char* text, int y_pos) {
    syscall(1, (uint64_t)text, (uint64_t)y_pos, 0);
}

/*
 * read_key() - Membaca satu karakter dari keyboard.
 *
 * Memanggil syscall nomor 3 (SYS_READ_KEY).
 * Non-blocking: mengembalikan 0 jika buffer keyboard kosong.
 *
 * Contoh penggunaan (menunggu input):
 *   char c;
 *   while ((c = read_key()) == 0) { sleep(10); }
 *   // 'c' sekarang berisi karakter yang ditekan
 *
 * @return: karakter ASCII atau 0 jika tidak ada input
 */
char read_key(void) {
    return (char)syscall(3, 0, 0, 0);
}
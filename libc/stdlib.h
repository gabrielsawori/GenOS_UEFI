#pragma once
#include <stdint.h>

/*
 * GenOS User-Space Standard Library (stdlib.h)
 *
 * Menyediakan fungsi utilitas dasar untuk aplikasi Ring-3.
 */

/* Mengakhiri aplikasi dengan kode status (syscall 2) */
void exit(int status);

/*
 * Menahan eksekusi selama N milidetik (syscall 4).
 * Timer PIT berdetak 1000x/detik, jadi sleep(1000) = tidur 1 detik.
 */
void user_sleep(uint32_t milliseconds);
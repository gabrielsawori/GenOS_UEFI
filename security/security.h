#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Kernel Security Module — GenOS v3
 *
 * Menyediakan mekanisme keamanan fundamental:
 *   1. SMEP (Supervisor Mode Execution Prevention) — mencegah kernel
 *      mengeksekusi kode di halaman user (anti-ret2user attack)
 *   2. SMAP (Supervisor Mode Access Prevention) — mencegah kernel
 *      membaca/menulis halaman user secara tidak sengaja
 *   3. Stack Canary — mendeteksi stack buffer overflow
 *   4. Secure Memory Wipe — menghapus data sensitif dari RAM
 *   5. User Pointer Validation — verifikasi pointer dari Ring 3
 *      sebelum kernel mengaksesnya
 *
 * PENTING: security_init() harus dipanggil setelah csprng_init()
 *          dan sebelum task_init().
 */

/* Inisialisasi modul keamanan. Aktifkan SMEP/SMAP jika CPU mendukung. */
void security_init(void);

/*
 * secure_memzero — Hapus data dari memori secara AMAN.
 *
 * Tidak seperti memset() biasa, fungsi ini DIJAMIN tidak akan dihapus
 * oleh compiler optimization. Digunakan untuk membersihkan:
 *   - Kunci kriptografi setelah selesai dipakai
 *   - Password buffers
 *   - Data sensitif lainnya
 *
 * Implementasi: volatile pointer write — compiler tidak bisa optimize away.
 */
void secure_memzero(void *ptr, size_t len);

/*
 * validate_user_ptr — Verifikasi pointer dari user-space (Ring 3).
 *
 * Memastikan pointer berada di rentang memori user-space yang valid
 * (lower half: 0x00000000_00000000 — 0x00007FFF_FFFFFFFF).
 * Mencegah serangan di mana user-space memberikan pointer ke kernel
 * memory untuk membaca/memodifikasi data kernel.
 *
 * @param ptr: Pointer yang akan diverifikasi
 * @return: 1 = valid (user-space), 0 = invalid (kernel-space atau NULL)
 */
int validate_user_ptr(const void *ptr);

/*
 * validate_user_buffer — Verifikasi bahwa seluruh buffer ada di user-space.
 *
 * @param ptr: Awal buffer
 * @param len: Panjang buffer
 * @return: 1 = seluruh buffer valid, 0 = invalid
 */
int validate_user_buffer(const void *ptr, size_t len);

/*
 * security_get_stack_canary — Dapatkan nilai stack canary random.
 * Dipanggil saat pembuatan kernel stack baru untuk task.
 */
uint64_t security_get_stack_canary(void);

/*
 * security_verify_canary — Verifikasi stack canary masih utuh.
 * Return: 1 = ok, 0 = STACK CORRUPTION DETECTED!
 */
int security_verify_canary(uint64_t expected, uint64_t actual);

/*
 * security_status — Cetak status fitur keamanan ke serial.
 */
void security_status(void);

#pragma once
#include <stdint.h>

/*
 * Capability-Based Access Control — GenOS v3
 *
 * Setiap proses memiliki bitmask capabilities yang menentukan
 * operasi apa saja yang diperbolehkan. Ini menerapkan prinsip
 * "Least Privilege" — proses hanya memiliki akses minimum yang
 * dibutuhkan untuk menjalankan tugasnya.
 *
 * Saat shell (PID pertama) diluncurkan, ia mendapat ALL capabilities.
 * Proses anak (child) mewarisi capabilities parent, tetapi parent
 * dapat mengurangi capabilities child saat exec().
 *
 * Capabilities bersifat one-way: hanya bisa DIKURANGI, tidak bisa
 * ditambah (kecuali oleh kernel sendiri).
 */

/* === Capability Bit Definitions === */

/* Akses filesystem: baca file */
#define CAP_FS_READ     (1U << 0)

/* Akses filesystem: tulis/buat/hapus file */
#define CAP_FS_WRITE    (1U << 1)

/* Eksekusi program lain (exec syscall) */
#define CAP_EXEC        (1U << 2)

/* Menggunakan cryptographic syscalls (hash, encrypt, dll) */
#define CAP_CRYPTO      (1U << 3)

/* Shared memory operations (create/attach/detach/destroy) */
#define CAP_SHM         (1U << 4)

/* Network access (reserved for future use) */
#define CAP_NET         (1U << 5)

/* Framebuffer/display access (draw_char, fill_rect, dll) */
#define CAP_DISPLAY     (1U << 6)

/* Process management (fork, clone, nice, dll) */
#define CAP_PROC        (1U << 7)

/* Hardware I/O access (reserved for future use) */
#define CAP_IO          (1U << 8)

/* Administrative capabilities (can grant/revoke caps to others) */
#define CAP_ADMIN       (1U << 9)

/* Raw device access (future: disk, PCI, etc) */
#define CAP_RAW_DEVICE  (1U << 10)

/* === Preset Capability Sets === */

/* Semua capabilities — diberikan ke shell/init process */
#define CAP_ALL         0x7FFU

/* Default untuk proses biasa — akses dasar tanpa admin/raw */
#define CAP_DEFAULT     (CAP_FS_READ | CAP_FS_WRITE | CAP_EXEC | \
                         CAP_CRYPTO | CAP_SHM | CAP_DISPLAY | CAP_PROC)

/* Minimal — hanya baca filesystem dan display */
#define CAP_MINIMAL     (CAP_FS_READ | CAP_DISPLAY)

/* Sandbox — tidak bisa exec, write, atau akses hardware */
#define CAP_SANDBOX     (CAP_FS_READ | CAP_DISPLAY | CAP_CRYPTO)

/* === API === */

/*
 * caps_init — Inisialisasi capability system. Dipanggil saat boot.
 */
void caps_init(void);

/*
 * caps_check — Cek apakah proses memiliki capability tertentu.
 *
 * @param task_caps: Bitmask capabilities proses (dari struct task)
 * @param required:  Capability yang dibutuhkan (bisa OR beberapa)
 * @return: 1 = punya semua yang diminta, 0 = ditolak
 */
int caps_check(uint32_t task_caps, uint32_t required);

/*
 * caps_grant — Tambahkan capability ke bitmask.
 * HANYA kernel yang boleh memanggil ini (tidak ada syscall untuk grant).
 *
 * @param current_caps: Pointer ke bitmask capabilities proses
 * @param new_cap:      Capability yang ditambahkan
 */
void caps_grant(uint32_t *current_caps, uint32_t new_cap);

/*
 * caps_revoke — Hapus capability dari bitmask.
 *
 * @param current_caps: Pointer ke bitmask capabilities proses
 * @param remove_cap:   Capability yang dihapus
 */
void caps_revoke(uint32_t *current_caps, uint32_t remove_cap);

/*
 * caps_inherit — Hitung capabilities untuk child process.
 * Child mewarisi capabilities parent, di-AND dengan mask.
 *
 * @param parent_caps: Capabilities parent
 * @param mask:        Mask pembatas (biasanya CAP_DEFAULT)
 * @return: Capabilities child
 */
uint32_t caps_inherit(uint32_t parent_caps, uint32_t mask);

/*
 * caps_to_string — Konversi bitmask ke string human-readable.
 * Tulis ke buf (minimal 128 byte).
 */
void caps_to_string(uint32_t caps, char *buf);

#include "security.h"
#include "../crypto/random.h"
#include "../drivers/serial.h"
#include "../kernel/utils.h"

/*
 * Kernel Security Module — GenOS v3
 *
 * Implementasi mekanisme keamanan hardware-assisted dan software.
 */

/* Batas atas user-space pada x86-64 canonical addressing */
#define USER_SPACE_TOP 0x00007FFFFFFFFFFF

/* Status flags — dicetak saat boot */
static int smep_enabled = 0;
static int smap_enabled = 0;
static int rdrand_available = 0;

/*
 * Cek CPUID flags untuk fitur keamanan CPU.
 * SMEP = CPUID.07H:EBX bit 7
 * SMAP = CPUID.07H:EBX bit 20
 */
static void detect_cpu_security_features(void) {
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 7, subleaf 0 */
    asm volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );

    /* SMEP: bit 7 of EBX */
    if (ebx & (1 << 7)) {
        serial_write_string("[SECURITY] CPU supports SMEP (Supervisor Mode Execution Prevention)\n");
        smep_enabled = 1;
    } else {
        serial_write_string("[SECURITY] CPU does NOT support SMEP\n");
    }

    /* SMAP: bit 20 of EBX */
    if (ebx & (1 << 20)) {
        serial_write_string("[SECURITY] CPU supports SMAP (Supervisor Mode Access Prevention)\n");
        smap_enabled = 1;
    } else {
        serial_write_string("[SECURITY] CPU does NOT support SMAP\n");
    }

    /* Check RDRAND (CPUID.01H:ECX bit 30) */
    asm volatile ("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
    if (ecx & (1 << 30)) {
        rdrand_available = 1;
    }
}

/*
 * Aktifkan SMEP dengan men-set CR4 bit 20.
 *
 * Efek: CPU akan memicu #GP (General Protection Fault) jika kode kernel
 * mencoba mengeksekusi instruksi di halaman yang ditandai User (U/S=1).
 * Ini mencegah serangan ret2usr di mana attacker mengarahkan RIP kernel
 * ke kode malicious di user-space.
 */
static void enable_smep(void) {
    if (!smep_enabled) return;

    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 20);  /* CR4.SMEP = bit 20 */
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    serial_write_string("[SECURITY] SMEP ENABLED — kernel cannot execute user pages\n");
}

/*
 * Aktifkan SMAP dengan men-set CR4 bit 21.
 *
 * Efek: CPU akan memicu #PF jika kode kernel mencoba membaca/menulis
 * halaman user TANPA menggunakan STAC/CLAC instructions.
 *
 * CATATAN: Untuk syscall handler yang perlu mengakses buffer user,
 * kita TIDAK mengaktifkan SMAP sepenuhnya karena syscall.c saat ini
 * langsung mengakses pointer user. Ini bisa ditambahkan nanti dengan
 * STAC/CLAC wrapper. Untuk sekarang, kita log fitur tapi tidak aktifkan.
 *
 * TODO: Refactor syscall.c untuk menggunakan copy_from_user()/copy_to_user()
 *       dengan STAC/CLAC, lalu aktifkan SMAP.
 */
static void enable_smap(void) {
    if (!smap_enabled) return;

    /*
     * SMAP diaktifkan tapi kita perlu berhati-hati:
     * Syscall handler GenOS saat ini langsung memproses pointer user
     * (misalnya `char* pesan = (char*)arg1`). Jika SMAP aktif,
     * semua akses ini akan #PF.
     *
     * Solusi sementara: Log bahwa SMAP tersedia, tapi belum diaktifkan.
     * Akan diaktifkan setelah refactor syscall.c.
     */
    serial_write_string("[SECURITY] SMAP available but DEFERRED — requires STAC/CLAC refactor\n");
    serial_write_string("[SECURITY] User pointer validation ACTIVE via software check\n");
}

/*
 * Aktifkan NX bit (No-Execute) pada halaman data.
 * NX sudah diaktifkan via EFER.NXE oleh Limine bootloader,
 * tapi kita verifikasi di sini.
 */
static void verify_nx_bit(void) {
    uint64_t efer;
    asm volatile ("rdmsr" : "=a"((uint32_t){0}), "=d"((uint32_t){0}) : "c"(0xC0000080));

    /* Baca EFER MSR secara benar */
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    efer = ((uint64_t)hi << 32) | lo;

    if (efer & (1ULL << 11)) {
        serial_write_string("[SECURITY] NX bit (No-Execute) is ENABLED\n");
    } else {
        serial_write_string("[SECURITY] WARNING: NX bit is disabled!\n");
        /* Aktifkan NX */
        efer |= (1ULL << 11);
        lo = (uint32_t)efer;
        hi = (uint32_t)(efer >> 32);
        asm volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));
        serial_write_string("[SECURITY] NX bit ENABLED via EFER.NXE\n");
    }
}

/* === Public API === */

void security_init(void) {
    serial_write_string("[INFO] Initializing Security Module...\n");
    serial_write_string("======================================\n");

    /* Deteksi fitur keamanan CPU */
    detect_cpu_security_features();

    /* Aktifkan proteksi hardware */
    enable_smep();
    enable_smap();
    verify_nx_bit();

    serial_write_string("======================================\n");
    serial_write_string("[OK] Security Module initialized!\n");
}

void secure_memzero(void *ptr, size_t len) {
    /*
     * Gunakan volatile pointer agar compiler TIDAK bisa
     * mengoptimasi penulisan ini (dead store elimination).
     * Ini krusial untuk membersihkan data kriptografi.
     */
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
    /*
     * Memory barrier — pastikan semua write sudah committed
     * sebelum fungsi return.
     */
    asm volatile ("" ::: "memory");
}

int validate_user_ptr(const void *ptr) {
    if (ptr == NULL) return 0;

    uint64_t addr = (uint64_t)ptr;

    /*
     * Pada x86-64, user-space menggunakan lower half:
     *   0x0000_0000_0000_0001 — 0x0000_7FFF_FFFF_FFFF
     *
     * Kernel menggunakan upper half (canonical high):
     *   0xFFFF_8000_0000_0000 — 0xFFFF_FFFF_FFFF_FFFF
     *
     * Alamat non-canonical (gap):
     *   0x0000_8000_0000_0000 — 0xFFFF_7FFF_FFFF_FFFF → #GP if accessed
     *
     * Kita tolak semua alamat di luar lower half yang valid.
     */
    if (addr > USER_SPACE_TOP) return 0;

    /* Tolak pointer ke page 0 (NULL page) untuk menangkap NULL deref */
    if (addr < 0x1000) return 0;

    return 1;
}

int validate_user_buffer(const void *ptr, size_t len) {
    if (len == 0) return 1;  /* Zero-length buffer selalu valid */
    if (!validate_user_ptr(ptr)) return 0;

    /* Cek bahwa end address juga masih di user-space */
    uint64_t end = (uint64_t)ptr + len - 1;
    if (end > USER_SPACE_TOP) return 0;

    /* Cek overflow */
    if (end < (uint64_t)ptr) return 0;

    return 1;
}

uint64_t security_get_stack_canary(void) {
    return csprng_get_u64();
}

int security_verify_canary(uint64_t expected, uint64_t actual) {
    if (expected != actual) {
        serial_write_string("[SECURITY] !!! STACK CANARY VIOLATION DETECTED !!!\n");
        serial_write_string("[SECURITY] Stack buffer overflow or corruption!\n");

        char buf[32];
        serial_write_string("[SECURITY] Expected: 0x");
        itoa(expected, buf, 16);
        serial_write_string(buf);
        serial_write_string("\n[SECURITY] Got:      0x");
        itoa(actual, buf, 16);
        serial_write_string(buf);
        serial_write_string("\n");

        return 0;
    }
    return 1;
}

void security_status(void) {
    serial_write_string("\n=== GenOS Security Status ===\n");
    serial_write_string("  SMEP:    ");
    serial_write_string(smep_enabled ? "ACTIVE" : "not available");
    serial_write_string("\n  SMAP:    ");
    serial_write_string(smap_enabled ? "available (deferred)" : "not available");
    serial_write_string("\n  NX bit:  ACTIVE\n");
    serial_write_string("  RDRAND:  ");
    serial_write_string(rdrand_available ? "available" : "not available");
    serial_write_string("\n  CSPRNG:  ACTIVE (ChaCha20-based)\n");
    serial_write_string("  Ptr validation: ACTIVE\n");
    serial_write_string("  Secure wipe:    ACTIVE\n");
    serial_write_string("=============================\n\n");
}

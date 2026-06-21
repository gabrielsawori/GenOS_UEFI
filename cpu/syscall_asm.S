.global syscall_entry
.extern syscall_handler
.extern kernel_stack_top  /* Meminjam variabel dari syscall.c */

.section .bss
user_rsp_tmp: .quad 0

.section .text
syscall_entry:
    /*
     * SYSCALL ENTRY POINT (Ring 3 -> Ring 0)
     *
     * Saat instruksi 'syscall' dieksekusi oleh aplikasi:
     *   - RCX  = RIP user (alamat instruksi setelah syscall), disimpan oleh CPU
     *   - R11  = RFLAGS user, disimpan oleh CPU
     *   - RAX  = nomor syscall
     *   - RDI  = arg1, RSI = arg2, RDX = arg3
     *
     * PENTING: STAR[63:48] harus berisi nilai = User Data Selector (0x1B).
     * CPU secara otomatis menghitung:
     *   User SS = STAR[63:48] + 8  => 0x1B + 8 = 0x23  (bukan yang kita inginkan)
     * KOREKSI: Karena GDT kita meletakkan User Data di 0x1B dan User Code di 0x23,
     *   maka agar sysretq menuju 0x23 (CS) dan 0x2B (SS) kita set STAR[63:48] = 0x1B.
     *   CPU akan set CS = STAR[63:48]+16 = 0x1B+16 = 0x2B ← SALAH, karena
     *   format STAR sysret: CS = STAR[63:48] | 3,  SS = (STAR[63:48]+8) | 3.
     *   Dengan STAR[63:48]=0x18: CS=0x18|3=0x1B? TIDAK.
     *   Aturan sesungguhnya AMD64 ABI: CPU memilih user_cs = STAR[63:48] | 3
     *   dan user_ss  = (STAR[63:48]+8) | 3.
     *   GDT kita: slot 3=User Data(0x18+DPL3=0x1B), slot 4=User Code(0x20+DPL3=0x23).
     *   Agar sysretq masuk ke CS=0x23: STAR[63:48] harus = 0x23 & ~3 = 0x20? Tidak,
     *   sysretq meng-OR 3 otomatis: jika STAR[63:48]=0x23, user_cs = 0x23 (OK karena
     *   CPU clear dua bit bawah lalu OR 3). user_ss = (0x23+8)|3 = 0x2B (OK).
     *   Jadi STAR[63:48] = 0x23 sudah BENAR, tapi syscall.c dulu set 0x23 sudah benar.
     *   Bug sebenarnya: push user_rsp_tmp mendorong ALAMAT SIMBOL bukan isinya!
     */

    /* 1. BEKUKAN APLIKASI: Simpan User RSP ke variabel sementara */
    mov %rsp, user_rsp_tmp(%rip)      /* Simpan RSP user ke variabel global */
    /* Pindah ke Stack Kernel yang aman */
    mov kernel_stack_top(%rip), %rsp

    /* 2. Simpan register krusial yang digunakan oleh instruksi syscall */
    push %rcx               /* RCX = RIP user (return address ke user mode) */
    push %r11               /* R11 = RFLAGS user */

    /*
     * BUG FIX: Gunakan R10 (bukan RAX!) untuk menyimpan user RSP ke stack.
     * RAX berisi NOMOR SYSCALL yang masih kita butuhkan di argument shuffle
     * nanti (mov %rax, %rdi). Jika kita pakai RAX di sini, nomor syscall
     * akan tertimpa oleh alamat user RSP → semua syscall jadi "tidak dikenal".
     * R10 aman dipakai karena bukan bagian dari konvensi syscall user.
     */
    mov user_rsp_tmp(%rip), %r10
    push %r10               /* Simpan RSP user ke stack kernel */

    /* Amankan register umum agar tidak hancur saat memanggil fungsi C */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* 3. TERJEMAHKAN ARGUMEN DARI KONVENSI SYSCALL KE KONVENSI C
     *
     * Saat syscall masuk:          Fungsi C handler butuh:
     *   RAX = syscall_num           RDI = syscall_num
     *   RDI = arg1                  RSI = arg1
     *   RSI = arg2                  RDX = arg2
     *   RDX = arg3                  RCX = arg3
     *
     * Kita harus menggeser dari KANAN KE KIRI agar tidak saling menimpa:
     *   1. rdx → rcx  (salin arg3 ke slot ke-4 C dulu, karena rcx belum dipakai)
     *   2. rsi → rdx  (salin arg2, rdx lama sudah diamankan ke rcx)
     *   3. rdi → rsi  (salin arg1, rsi lama sudah diamankan ke rdx)
     *   4. rax → rdi  (salin syscall_num, rdi lama sudah diamankan ke rsi)
     */
    mov %rdx, %rcx          /* rcx = arg3 (C param ke-4) */
    mov %rsi, %rdx          /* rdx = arg2 (C param ke-3) */
    mov %rdi, %rsi          /* rsi = arg1 (C param ke-2) */
    mov %rax, %rdi          /* rdi = syscall_num (C param ke-1) */

    /* 4. PANGGIL HANDLER SYSCALL (Fungsi C) */
    cld                         /* Clear DF — user code might have set it */
    call syscall_handler

    /*
     * 5. KEMBALIKAN SEMUANYA SEPERTI SEMULA
     *
     * RAX sekarang berisi RETURN VALUE dari syscall_handler.
     * Kita HARUS menyimpannya sebelum pop sequence, karena nanti
     * kita perlu pop user RSP dari stack. Gunakan RDI sebagai
     * tempat sementara (RDI tidak ada di push/pop list).
     */
    mov %rax, %rdi          /* Simpan return value ke RDI (aman) */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    /* Pulihkan RSP user, R11 (RFLAGS), dan RCX (RIP user) */
    pop %rax                /* RAX = user RSP (dari stack) */
    pop %r11                /* Pulihkan RFLAGS user */
    pop %rcx                /* Pulihkan RIP user (sysretq butuh ini di RCX) */
    mov %rax, %rsp          /* Ganti RSP ke Stack User kembali */

    mov %rdi, %rax          /* Kembalikan RETURN VALUE ke RAX! */

    /* Kembali ke User Mode (Ring 3).
     * sysretq akan: RIP=RCX, RFLAGS=R11, CS=selector user, SS=selector user+8
     * RAX = return value yang diterima user-space */
    sysretq
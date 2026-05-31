.global syscall_entry
.extern syscall_handler
.extern kernel_stack_top  /* Meminjam variabel dari syscall.c */

.section .bss
user_rsp_tmp: .quad 0

.section .text
syscall_entry:
    /* 1. BEKUKAN APLIKASI: Simpan ruang kerja aplikasi (User Stack) */
    mov %rsp, user_rsp_tmp
    /* Pindah ke "Kasur Empuk" (Stack) khusus Kernel agar aman */
    mov kernel_stack_top, %rsp

    /* 2. Simpan register krusial yang digunakan oleh instruksi syscall */
    push %rcx    
    push %r11    
    push user_rsp_tmp 

    /* Amankan register umum agar tidak hancur saat kita memanggil fungsi C */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* 3. TERJEMAHKAN ARGUMEN UNTUK BAHASA C */
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi

    /* 4. PANGGIL PENJAGA GERBANG TOL (Fungsi C) */
    call syscall_handler

    /* 5. KEMBALIKAN SEMUANYA SEPERTI SEMULA */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    pop %rax       
    pop %r11
    pop %rcx
    mov %rax, %rsp 

    /* Instruksi ajaib untuk kembali ke User Mode (Ring 3) */
    sysretq
.global syscall_entry
.extern syscall_handler
.extern kernel_stack_top

.section .bss
user_rsp_tmp: .quad 0

.section .text
syscall_entry:
    /*
     * SYSCALL ENTRY POINT (Ring 3 -> Ring 0)
     *
     * Saat instruksi 'syscall' dieksekusi oleh aplikasi:
     *   - RCX  = RIP user (return address)
     *   - R11  = RFLAGS user
     *   - RAX  = nomor syscall
     *   - RDI  = arg1, RSI = arg2, RDX = arg3
     */

    /* 1. Simpan User RSP dan pindah ke Kernel Stack */
    mov %rsp, user_rsp_tmp(%rip)
    mov kernel_stack_top(%rip), %rsp

    /* 2. Simpan register krusial */
    push %rcx               /* RCX = RIP user */
    push %r11               /* R11 = RFLAGS user */

    mov user_rsp_tmp(%rip), %r10
    push %r10               /* Simpan RSP user ke stack kernel */

    /* Amankan register callee-saved */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* 3. Terjemahkan argumen syscall -> konvensi C */
    mov %rdx, %rcx          /* rcx = arg3 (C param ke-4) */
    mov %rsi, %rdx          /* rdx = arg2 (C param ke-3) */
    mov %rdi, %rsi          /* rsi = arg1 (C param ke-2) */
    mov %rax, %rdi          /* rdi = syscall_num (C param ke-1) */

    /* 4. Panggil handler */
    cld
    call syscall_handler

    /* 5. Kembalikan semuanya */
    mov %rax, %rdi          /* Simpan return value */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    pop %rax                /* RAX = user RSP */
    pop %r11                /* RFLAGS user */
    pop %rcx                /* RIP user */
    mov %rax, %rsp          /* Kembali ke User Stack */

    mov %rdi, %rax          /* Return value ke RAX */

    sysretq
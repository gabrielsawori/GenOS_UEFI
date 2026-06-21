.extern isr_handler
.extern log_bad_cs

/* Makro untuk CPU Error (Tanpa Kode) */
.macro ISR_NOERRCODE i
.global isr\i
isr\i:
    push $0
    push $\i
    jmp isr_common
.endm

/* Makro untuk CPU Error (Dengan Kode) */
.macro ISR_ERRCODE i
.global isr\i
isr\i:
    push $\i
    jmp isr_common
.endm

/* Makro untuk Hardware Interrupt (IRQ) */
.macro IRQ_STUB i, irq_num
.global irq\irq_num
irq\irq_num:
    push $0
    push $\i
    jmp isr_common
.endm

/* 32 Error Bawaan CPU (INT 0 - 31) */
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

/* 16 Hardware IRQ (INT 32 - 47) */
IRQ_STUB 32, 0
IRQ_STUB 33, 1
IRQ_STUB 34, 2
IRQ_STUB 35, 3
IRQ_STUB 36, 4
IRQ_STUB 37, 5
IRQ_STUB 38, 6
IRQ_STUB 39, 7
IRQ_STUB 40, 8
IRQ_STUB 41, 9
IRQ_STUB 42, 10
IRQ_STUB 43, 11
IRQ_STUB 44, 12
IRQ_STUB 45, 13
IRQ_STUB 46, 14
IRQ_STUB 47, 15

/* LAPIC Timer Interrupt (INT 48) */
.global lapic_timer_stub
lapic_timer_stub:
    push $0
    push $48
    jmp isr_common

/* Menyimpan semua register */
isr_common:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /*
     * BUG FIX (Bare Metal): Clear direction flag.
     * Some BIOS/UEFI firmwares leave DF=1. If an interrupt fires while
     * DF=1 and the C handler uses memcpy/memset/string operations,
     * they go backwards and corrupt memory. QEMU always starts with
     * DF=0, masking this bug.
     */
    cld

    mov %rsp, %rdi
    call isr_handler

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax

    add $16, %rsp

    /*
     * DEFENSIVE CS/SS VALIDATION (Bare Metal Safety Net)
     *
     * Before iretq, verify CS and SS are consistent. In x86-64 long mode,
     * iretq ALWAYS pops RIP, CS, RFLAGS, RSP, SS — and validates that
     * SS.DPL == target CPL (from CS). If CS and SS mismatch in privilege
     * level, iretq triggers #GP with the SS selector as error code.
     *
     * STACK LAYOUT after "push %rax":
     *     [RSP+0]  = RAX (saved)
     *     [RSP+8]  = RIP
     *     [RSP+16] = CS
     *     [RSP+24] = RFLAGS
     *     [RSP+32] = RSP
     *     [RSP+40] = SS
     */
    push %rax
    mov 16(%rsp), %ax         /* Read CS from stack */
    cmp $0x08, %ax
    je .Lcs_kernel            /* Kernel CS — validate SS=0x10 */
    cmp $0x23, %ax
    je .Lcs_user              /* User CS   — validate SS=0x1B */

    /* CS is GARBAGE — log and fix to kernel */
.Lcs_bad:
    push %rdi
    push %rsi
    push %rcx
    push %rdx
    push %r8
    push %r9
    mov %rax, %rdi            /* bad CS value */
    call log_bad_cs
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    pop %rsi
    pop %rdi

    /* Force kernel mode: CS=0x08, SS=0x10 */
    movw $0x08, 16(%rsp)
    movw $0x10, 40(%rsp)
    jmp .Lcs_done

.Lcs_kernel:
    /* CS=0x08 (kernel): SS MUST be 0x10. Force it.
     * If SS were 0x1B (user data, DPL=3), iretq to CPL=0 would
     * #GP(0x18) because SS.DPL(3) != target CPL(0). */
    movw $0x10, 40(%rsp)
    jmp .Lcs_done

.Lcs_user:
    /* CS=0x23 (user): SS MUST be 0x1B. Force it.
     * If SS were 0x10 (kernel data, DPL=0), iretq to CPL=3 would
     * #GP because SS.DPL(0) != target CPL(3). */
    movw $0x1B, 40(%rsp)

.Lcs_done:
    pop %rax
    iretq

/* Tabel Array Error CPU */
.global isr_stub_table
isr_stub_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

/* Tabel Array Hardware IRQ */
.global irq_stub_table
irq_stub_table:
    .quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    .quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
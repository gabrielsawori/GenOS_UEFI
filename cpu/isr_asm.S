.extern isr_handler

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
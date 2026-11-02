.global syscall_entry
.extern syscall_handler

.section .text
syscall_entry:
    /* IF is masked by FMASK. Offsets asserted in percpu.c. */
    swapgs
    mov %rsp, %gs:8
    mov %gs:16, %rsp

    /* Build the same 176-byte struct registers as interrupt entry. This is
     * also fork's user return frame; never borrow an old timer snapshot.
     */
    push $0x1b
    pushq %gs:8
    push %r11
    push $0x23
    push %rcx
    push $0
    push $0
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

    /* C(num, arg1, arg2, arg3, frame). RSP is aligned before CALL. */
    mov %rsp, %r8
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi
    cld
    call syscall_handler
    cli
    mov %rax, 112(%rsp)

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

    mov 0(%rsp), %rcx
    mov 16(%rsp), %r11
    mov 24(%rsp), %rsp
    swapgs
    sysretq

.section .note.GNU-stack,"",@progbits

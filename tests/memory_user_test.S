/* Real user/kernel write faults, shared visibility, and rejected RO/NX access. */
.global _start
.section .text
_start:
    /* A real exec must pass the TAR payload's declared length to the loader. */
    lea truncated_name(%rip), %rdi
    mov $9, %eax
    syscall
    cmp $-1, %rax
    jne failed
    mov $17, %eax
    mov $4096, %edi
    syscall
    test %rax, %rax
    js failed
    mov %rax, %rdi
    mov $18, %eax
    syscall
    test %rax, %rax
    jz failed
    mov %rax, %r13
    mov $22, %eax
    syscall
    test %rax, %rax
    js failed
    jz child

    movq $22, private_value(%rip)
wait_child:
    cmpq $99, (%r13)
    jne wait_child
    cmpq $22, private_value(%rip)
    jne failed
    mov $2, %edi
    mov $0x7fff, %eax
    syscall

    mov $22, %eax
    syscall
    test %rax, %rax
    js failed
    jz probe_ro
    mov $22, %eax
    syscall
    test %rax, %rax
    js failed
    jz probe_nx
    jmp done

child:
    /* Kernel writes into a still-CoW data page via the production syscall. */
    lea screen_info(%rip), %rdi
    mov $31, %eax
    syscall
    test %rax, %rax
    jnz failed
    cmpq $11, private_value(%rip)
    jne failed
    movq $44, private_value(%rip)
    push %rax
    pop %rax
    mov $1, %edi
    mov $0x7fff, %eax
    syscall
    movq $99, (%r13)
done:
    mov $2, %eax
    syscall
    ud2

probe_ro:
    mov $1, %edi
    lea readonly_value(%rip), %rsi
    mov $0x7ffe, %eax
    syscall
    movq $1, readonly_value(%rip)
    jmp failed
probe_nx:
    mov $2, %edi
    lea nx_byte(%rip), %rsi
    mov $0x7ffe, %eax
    syscall
    lea nx_byte(%rip), %rax
    call *%rax
failed:
    xor %edi, %edi
    mov $0x7fff, %eax
    syscall
    ud2

.section .rodata
truncated_name: .asciz "truncated.elf"
.balign 8
readonly_value: .quad 0
.section .data
.balign 8
private_value: .quad 11
screen_info: .quad 0
nx_byte: .byte 0xc3
.section .note.GNU-stack,"",@progbits

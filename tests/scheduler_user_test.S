/* Ring-3 probe for the real SYSCALL/SYSRET and fork/IRET paths. Avoid private
 * page writes here: CoW write ownership is explicitly a later milestone.
 */
.global _start
.section .text
_start:
    mov $0x12345678, %r12
    mov $0x23456789, %rbx
    mov $29, %rax
    syscall
    cmp $1, %rax
    jne failed
    cmp $0x12345678, %r12
    jne failed
    cmp $0x23456789, %rbx
    jne failed
    mov $22, %rax
    syscall
    test %rax, %rax
    js failed
    mov $1, %edi
    jz report
    mov $2, %edi
report:
    cmp $0x12345678, %r12
    jne failed
    cmp $0x23456789, %rbx
    jne failed
    mov $0x7fff, %rax
    syscall
    mov $2, %rax
    syscall
failed:
    xor %edi, %edi
    mov $0x7fff, %rax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits

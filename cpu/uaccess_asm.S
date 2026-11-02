/* Only the user-facing memory instruction has a recovery entry. A bug in
 * the kernel source/destination remains fatal instead of being concealed.
 */
.text
.global uaccess_copy_from_asm
.type uaccess_copy_from_asm, @function
uaccess_copy_from_asm:
    test %rdx, %rdx
    jz .Lfrom_done
.Lfrom_load:
    movb (%rsi), %al
    movb %al, (%rdi)
    inc %rsi
    inc %rdi
    dec %rdx
    jnz .Lfrom_load
.Lfrom_done:
    xor %eax, %eax
    ret
.Lfrom_fault:
    mov $-1, %eax
    ret
.size uaccess_copy_from_asm, .-uaccess_copy_from_asm

.global uaccess_copy_to_asm
.type uaccess_copy_to_asm, @function
uaccess_copy_to_asm:
    test %rdx, %rdx
    jz .Lto_done
.Lto_load:
    movb (%rsi), %al
.Lto_store:
    movb %al, (%rdi)
    inc %rsi
    inc %rdi
    dec %rdx
    jnz .Lto_load
.Lto_done:
    xor %eax, %eax
    ret
.Lto_fault:
    mov $-1, %eax
    ret
.size uaccess_copy_to_asm, .-uaccess_copy_to_asm

.section .rodata
.balign 8
.global uaccess_fixups_start, uaccess_fixups_end
uaccess_fixups_start:
    .quad .Lfrom_load, .Lfrom_fault, 0
    .quad .Lto_store, .Lto_fault, 1
uaccess_fixups_end:
.section .note.GNU-stack,"",@progbits

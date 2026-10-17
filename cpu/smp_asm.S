.section .text
.global smp_ap_start
.type smp_ap_start, @function

/* RDI = permanent stack top, RSI = struct ap_boot_info pointer.
 * Abandon the bootloader stack, then CALL C with the SysV stack alignment.
 * This function never returns to its caller on the old stack.
 */
smp_ap_start:
    cli
    cld
    mov %rdi, %rsp
    and $-16, %rsp
    xor %ebp, %ebp
    mov %rsi, %rdi
    call smp_ap_main

    /* smp_ap_main is noreturn; halt if that contract is ever broken. */
    cli
1:
    hlt
    jmp 1b
.size smp_ap_start, .-smp_ap_start

.section .note.GNU-stack,"",@progbits

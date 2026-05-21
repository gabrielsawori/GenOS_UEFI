.global gdt_load

gdt_load:
    /* Register RDI berisi alamat 'gdtr' yang kita kirim dari C tadi */
    lgdt (%rdi)

    /* Memuat ulang register segmen data dengan Kernel Data (Offset 0x10 dari tabel) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Trik sulap untuk memuat ulang register CS (Kernel Code = Offset 0x08) */
    /* Kita melakukan lompatan jauh (Far Return) */
    pop %rdi         /* Simpan alamat kembali (return address) asli */
    push $0x08       /* Dorong segmen kode kernel ke stack */
    push %rdi        /* Dorong alamat kembali ke atasnya */
    lretq            /* CPU akan melompat dan mengatur CS secara otomatis! */
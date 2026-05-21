.global idt_load

idt_load:
    /* Register RDI berisi alamat 'idtr' yang dikirim dari kode C */
    lidt (%rdi)
    ret
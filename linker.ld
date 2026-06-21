OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ;
    rodata  PT_LOAD    FLAGS((1 << 2)) ;
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ;
    dynamic PT_DYNAMIC FLAGS((1 << 1) | (1 << 2)) ;
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    /* ====================================================================
     * LIMINE v5.x BOOT PROTOCOL: Request markers + requests section.
     *
     * Limine v5.x bootloader WAJIB menemukan dua marker (start & end)
     * yang membungkus section .requests. Jika marker hilang, Limine
     * TIDAK akan memproses request apapun — semua response akan NULL.
     * Akibatnya kernel hang di hcf() saat cek framebuffer/memmap.
     *
     * Marker & requests DIBUTUHKAN (KEEP) agar tidak ter-garbage collect.
     * ==================================================================== */
    . = ALIGN(8);
    .requests_start_marker : {
        KEEP(*(.requests_start_marker))
    } :data

    .requests : {
        KEEP(*(.requests))
    } :data

    .requests_end_marker : {
        KEEP(*(.requests_end_marker))
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}
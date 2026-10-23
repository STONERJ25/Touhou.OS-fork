/* Gensokyo kernel: higher-half ELF64, three page-aligned segments (R-X, R--, RW-). */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_BASE = 0xFFFFFFFF80000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    ksyms  PT_LOAD FLAGS(4);
}

SECTIONS
{
    . = KERNEL_BASE;
    __kernel_start = .;

    .text ALIGN(4096) : {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    } :text

    .rodata ALIGN(4096) : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __initcall_start = .;
        KEEP(*(.initcall))
        __initcall_end = .;
        . = ALIGN(4096);
        __rodata_end = .;
    } :rodata

    .data ALIGN(4096) : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(8);
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    } :data

    /* Symbol table for backtraces; last on purpose so its size never moves any other symbol. */
    .ksyms ALIGN(4096) : {
        __ksyms_start = .;
        KEEP(*(.ksyms))
        . = ALIGN(8);
        __ksyms_end = .;
    } :ksyms

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

/* Static user programs: three page-aligned segments (R-X, R--, RW-), loaded at 4 MiB. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;

    .text ALIGN(4096) : {
        *(.text .text.*)
    } :text

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(4096) : {
        *(.data .data.*)
        . = ALIGN(8);
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

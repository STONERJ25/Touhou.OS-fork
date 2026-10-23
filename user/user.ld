/* Static user programs: three page-aligned segments (R-X, R--, RW-), loaded at 4 MiB, plus the PT_TLS template that
 * _Thread_local variables live in (.tdata is copied into every new thread's TLS block, .tbss starts zeroed). */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    tls    PT_TLS  FLAGS(4);
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

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.*)
        *(.tcommon)
    } :data :tls

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
    } :data

    /* what libc's thread start-up needs to build a TLS block (see user/libc/src/pthread.c) */
    __tls_init_image = ADDR(.tdata);
    __tls_init_size  = SIZEOF(.tdata);
    __tls_mem_size   = (ADDR(.tbss) - ADDR(.tdata)) + SIZEOF(.tbss);
    __tls_align      = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

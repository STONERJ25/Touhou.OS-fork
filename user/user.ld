/* Static user programs: three page-aligned segments (R-X, R--, RW-), loaded at 4 MiB, plus the PT_TLS template that
 * _Thread_local variables live in (.tdata is copied into every new thread's TLS block, .tbss starts zeroed).
 *
 * Every program also carries an ELF note that says "I am a native TouhouOS program": name "Gensokyo", type 1 (NT_GENSOKYO_ABI), and the number of this system call
 * interface's generation as its descriptor. The kernel reads it at execve (kernel/linux/loader.c, linux_elf_mark; docs/COMPAT.md, "The personality is chosen by the
 * file") to pick the system call personality from the file instead of from the program that starts it. The note is written out byte by byte here because the
 * programs are linked without a start-up file that could carry it; PT_NOTE points at it and it sits in the read-only segment. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    tls    PT_TLS  FLAGS(4);
    note   PT_NOTE FLAGS(4);
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

    .note.gensokyo : ALIGN(4) {
        LONG(9)                                     /* namesz: "Gensokyo" and its NUL */
        LONG(4)                                     /* descsz */
        LONG(1)                                     /* type: NT_GENSOKYO_ABI */
        BYTE(0x47) BYTE(0x65) BYTE(0x6e) BYTE(0x73) /* G e n s */
        BYTE(0x6f) BYTE(0x6b) BYTE(0x79) BYTE(0x6f) /* o k y o */
        BYTE(0x00) BYTE(0x00) BYTE(0x00) BYTE(0x00) /* the NUL, then padding to a multiple of 4 */
        LONG(1)                                     /* desc: generation 1 of the native system call interface */
    } :rodata :note

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

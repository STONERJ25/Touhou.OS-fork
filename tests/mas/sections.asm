bits 64
section .text
    nop
section .data
dd 1
section .rodata
db 2
section .bss
resb 16
section .text
    ret
section .note progbits noalloc noexec nowrite align=4
dd 5
section .special progbits alloc write noexec align=32
dq 9
section .tdata progbits alloc write
dd 3
section .init_array progbits alloc noexec write align=8
dq 1
section .text
    nop

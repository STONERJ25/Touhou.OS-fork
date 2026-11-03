; included by tests/mas/include.asm
FIRST equ 0x10
SECOND equ FIRST + 2
%macro emit_pair 2
    mov dword [rdi], %1
    mov dword [rdi + 4], %2
%endmacro

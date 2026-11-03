; %include with a file found through -I, equ constants from it, a macro from it, incbin of a file next to the include path
bits 64
default rel
%include "mas-defs.inc"
section .text
global main
main:
    mov eax, FIRST
    add rax, SECOND * 2
    emit_pair 7, 9
    lea rsi, [blob]
    mov ecx, blob_end - blob
section .rodata
blob:
    incbin "mas-blob.bin"
blob_end:
    dq main

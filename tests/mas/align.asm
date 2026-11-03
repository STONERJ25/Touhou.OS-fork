; align and alignb in code and data, with every amount of padding from 0 to 15, times, resb/dq/incbin-free data, $-relative sizes
bits 64
section .text
global start
start:
    nop
    align 16
    nop
    nop
    align 16
    times 3 nop
    align 8
    mov eax, 1
    align 16
    ret
    align 4
    ret
    times 15 nop
    align 16
    times 7 nop
    align 32
    jmp start
    jmp short start
section .data
    align 8
    db 1
    align 8
    db 2, 3
    alignb 8
    dd 0
    align 4
    dq start
section .bss
    resb 3
    alignb 16
    resq 2

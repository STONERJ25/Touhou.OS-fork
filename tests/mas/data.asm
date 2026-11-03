bits 64
default rel
section .data
align 16
global d_start
d_start:
    db 1, 2, 3
    db 'abc', 0
    db "hello", 10, 0
    dw 0x1234, -1, 65535
    dd 0x12345678, -1, 4294967295
    dq 0x123456789abcdef0, -1
    dq d_start
    dq d_end
    dd d_start
    dw 'ab'
    dd 'abcd'
    dq 'abcdefgh'
    db 'x', 'y', 'z'
    dd d_end - d_start
    dq d_end - d_start + 4
d_end:
    times 4 db 0xAA
    times 2 dq 7
    align 8
    db 1
    align 4
    db 2
    alignb 16
values: dq values, $, $$
    dd $ - values
    dw (1 + 2) * 3, 7 / 2, 7 % 4, 1 << 4, 0xff >> 4, 5 & 3, 5 | 3, 5 ^ 3, ~5 & 0xff, -(3)
section .rodata
msg: db 'rodata string', 0
ptr: dq msg
section .bss
alignb 8
buf: resb 64
words: resw 3
dwords: resd 2
qwords: resq 5
global bss_end
bss_end:
section .text
global code
code:
    lea rax, [msg]
    lea rbx, [rel buf]
    mov rcx, [ptr]
    mov eax, msg
    mov rax, msg
    lea rdx, [abs msg]
    mov dword [rel d_start], 1
    mov rax, [bss_end]
    mov eax, [abs buf]
    push msg
    add rax, msg
    cmp rax, msg
    mov byte [buf + 3], 9
    mov qword [rel words], 5

bits 64
default rel
extern ext_a, ext_b
extern ext_c
global g1, g2
section .text
g1: nop
local1: nop
.dot: nop
g2: nop
.dot2: jmp .dot2
global later
later:
    nop
    call ext_a
    lea rax, [ext_b]
    mov rax, [ext_c + 8]
    mov rax, ext_a
    mov eax, ext_b
    add rax, ext_c
    jmp ext_b
    push ext_a
    mov [ext_c], rax
    cmp dword [ext_a + 4], 1
    mov [rax + ext_c], rbx
section .data
ptrs: dq ext_a, ext_b + 8, g1, local1, later
    dd ext_c
VALUE equ 42
SIZE equ end - start
start: dq VALUE
end:
ALIAS equ g2
    dq ALIAS
    dq SIZE
AFTER equ $ - ptrs

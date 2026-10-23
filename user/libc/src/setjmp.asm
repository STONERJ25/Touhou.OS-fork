bits 64
default rel

section .text

; int setjmp(jmp_buf env): saves rbx rbp r12-r15 rsp rip
global setjmp
setjmp:
    mov [rdi], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    mov rax, [rsp]
    mov [rdi + 56], rax
    xor eax, eax
    ret

; void longjmp(jmp_buf env, int val)
global longjmp
longjmp:
    mov eax, esi
    test eax, eax
    jnz .go
    inc eax
.go:
    mov rbx, [rdi]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    jmp [rdi + 56]

bits 64
section .text
    push rax
    pop rax
    push rcx
    pop rcx
    push rdx
    pop rdx
    push rbx
    pop rbx
    push rsp
    pop rsp
    push rbp
    pop rbp
    push rsi
    pop rsi
    push rdi
    pop rdi
    push r8
    pop r8
    push r9
    pop r9
    push r10
    pop r10
    push r11
    pop r11
    push r12
    pop r12
    push r13
    pop r13
    push r14
    pop r14
    push r15
    pop r15
    push 0
    push 1
    push -1
    push 127
    push 128
    push -128
    push -129
    push 0x12345678
    push qword 5
    push qword 0x80
    push qword [rax]
    push qword [rsp + 8]
    pop qword [rbx]
    pop qword [rbp - 8]

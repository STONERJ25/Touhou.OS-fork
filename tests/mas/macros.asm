bits 64
section .text
%define ONE 1
%assign n 3
%assign n n * 2
%macro two_nops 0
    nop
    nop
%endmacro
%macro load 2
    mov %1, %2
%endmacro
%macro tagged 1
tag_%+%1:
    mov eax, %1
%endmacro
%macro loopy 1
%%again:
    dec %1
    jnz %%again
%endmacro
    mov eax, ONE
    mov ebx, n
    two_nops
    load rax, rbx
    load rcx, 5
    load qword [rdi], 7
    tagged 1
    tagged 2
    loopy rcx
    loopy rdx
%rep 4
    nop
%endrep
%assign i 0
%rep 5
    mov eax, i
%assign i i + 1
%endrep
%if n == 6
    db 'six'
%elif n == 7
    db 'seven'
%else
    db 'other'
%endif
%ifdef ONE
    db 'one defined'
%endif
%ifndef TWO
    db 'two undefined'
%endif
%if (n > 5 && n < 9) || n == 100
    db 'range'
%endif
%rep 3
%assign j 0
%rep 2
    dd j + i
%assign j j + 1
%endrep
%endrep
%macro pushall 0
%assign r 0
%rep 3
    push rax
%assign r r + 1
%endrep
%endmacro
    pushall
label_%+n: nop
    jmp label_6

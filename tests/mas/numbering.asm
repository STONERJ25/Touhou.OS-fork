; The numbers in the names NASM gives to the %%local labels of macro calls (..@N.name, they are in the object file's symbol table, and mas must write the same bytes): a counter
; from 0 that every macro call takes a number from, as does each directive that NASM implements as a standard macro: one for bits, default, cpu, section, segment, extern, global
; and incbin, two for align and alignb. A macro called from a macro takes the next number after its caller's.
bits 64
default rel
cpu x64
extern ext_a, ext_b
global first_one
%macro guard 0
%%skip:
    nop
%endmacro
%macro tag 0
%%t:
%endmacro
%macro outer 0
%%top:
    guard
%%bottom:
    nop
%endmacro
section .text
first_one:
    guard
    align 16
    guard
    outer
    call ext_a
    guard
section .data
    align 8
    dq 1
    incbin "mas-blob.bin"
    guard
section .bss
    alignb 32
buf: resb 8
    tag
segment .text
    outer
    align 4
    jmp ext_b
    guard

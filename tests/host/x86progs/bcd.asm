; Decimal arithmetic: 8-digit packed BCD addition (ADC + DAA) and subtraction (SBB + DAS) with the carry carried through
; INC and LOOP, then AAM/AAD on 7*8 and the ASCII adjustments AAA ('9'+'5') and AAS ('3'-'7').
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286
; expect-mem: 1000:0400 07 00 00 00 01 00 00 00 22 43 65 37 06 05 38 00 04 01 06 ff
        cpu 8086
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        mov si, a1              ; 12345678 + 87654329 = 100000007
        mov bx, b1
        mov di, r_add
        mov cx, 4
        clc
addl:   mov al, [si]
        adc al, [bx]
        daa
        mov [di], al
        inc si
        inc bx
        inc di
        loop addl
        mov al, 0
        adc al, 0
        mov [di], al
        mov si, c1              ; 50000000 - 12345678 = 37654322
        mov bx, a1
        mov di, r_sub
        mov cx, 4
        clc
subl:   mov al, [si]
        sbb al, [bx]
        das
        mov [di], al
        inc si
        inc bx
        inc di
        loop subl
        mov al, 7
        mov bl, 8
        mul bl
        aam                     ; 56 -> AH = 5, AL = 6
        mov [r_aam], ax
        aad                     ; back to 38h
        mov [r_aad], ax
        mov ax, '9'
        add al, '5'
        aaa                     ; 0104h
        mov [r_aaa], ax
        mov ax, '3'
        sub al, '7'
        aas                     ; FF06h
        mov [r_aas], ax
        hlt

a1:     db 78h, 56h, 34h, 12h
b1:     db 29h, 43h, 65h, 87h
c1:     db 00h, 00h, 00h, 50h

        times 300h - ($ - $$) db 0
r_add:  times 8 db 0            ; 1000:0400
r_sub:  dd 0                    ; 1000:0408
r_aam:  dw 0                    ; 1000:040C
r_aad:  dw 0
r_aaa:  dw 0
r_aas:  dw 0

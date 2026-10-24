; Multiply and divide: 8! with MUL, 123456789 printed in decimal with chained 32/16-bit DIVs, Euclid's GCD with DIV,
; IMUL and IDIV with negative operands (word and byte), and the CF/OF a byte MUL leaves.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286
; expect-mem: 1000:0400 80 9d 31 32 33 34 35 36 37 38 39 00 15 00 cc f7 ff ff 2e ff ff ff 58 02 01 08 f2 fe
        cpu 8086
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        mov ax, 1               ; 8! = 40320 = 9D80h
        mov cx, 8
fact:   mul cx
        loop fact
        mov [r_fact], ax
        mov dx, 075Bh           ; 123456789 = 075BCD15h
        mov ax, 0CD15h
        mov di, r_dec + 8
        mov bx, 10
        mov cx, 9
digit:  push ax
        mov ax, dx
        xor dx, dx
        div bx                  ; high word / 10
        mov si, ax
        pop ax
        div bx                  ; (remainder : low word) / 10
        add dl, '0'
        mov [di], dl
        dec di
        mov dx, si
        loop digit
        mov ax, 1071            ; gcd(1071, 462) = 21
        mov bx, 462
gcd:    xor dx, dx
        div bx
        mov ax, bx
        mov bx, dx
        or bx, bx
        jnz gcd
        mov [r_gcd], ax
        mov ax, -300            ; -300 * 7 = -2100
        mov cx, 7
        imul cx
        mov [r_imul], ax
        mov [r_imul+2], dx
        mov ax, -2101           ; -2101 / 10 = -210 remainder -1
        cwd
        mov cx, 10
        idiv cx
        mov [r_idiv], ax
        mov [r_idiv+2], dx
        mov al, 200             ; 200 * 3 = 600: does not fit a byte, CF = OF = 1
        mov bl, 3
        mul bl
        mov [r_mulb], ax
        pushf
        pop ax
        and ax, 0801h
        mov [r_mulf], ax
        mov ax, -100            ; -100 / 7 = -14 remainder -2
        mov bl, 7
        idiv bl
        mov [r_idivb], ax
        hlt

        times 300h - ($ - $$) db 0
r_fact:  dw 0                   ; 1000:0400
r_dec:   times 10 db 0          ; 1000:0402
r_gcd:   dw 0                   ; 1000:040C
r_imul:  dd 0                   ; 1000:040E
r_idiv:  dd 0                   ; 1000:0412
r_mulb:  dw 0                   ; 1000:0416
r_mulf:  dw 0                   ; 1000:0418
r_idivb: dw 0                   ; 1000:041A

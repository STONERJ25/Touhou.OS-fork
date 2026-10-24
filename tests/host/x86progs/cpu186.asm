; The 80186 additions the V30 and the 286 share: PUSHA/POPA, PUSH imm16/imm8, IMUL r, r/m, imm (with overflow), shifts by
; an immediate, ENTER/LEAVE (nesting levels 0 and 2), BOUND in range.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: v30 286
; expect: ax=fffb bx=1234 cx=369c dx=4000 si=000f di=0005 bp=abcd sp=fffe
; expect-mem: 1000:0400 11 11 22 22 33 33 44 44 fe ff 55 55 66 66 77 77 01 08 ef be cd ab fa ff ff ff
        cpu 186
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0FFFEh
        mov ax, 1111h
        mov cx, 2222h
        mov dx, 3333h
        mov bx, 4444h
        mov bp, 5555h
        mov si, 6666h
        mov di, 7777h
        pusha
        xor ax, ax
        xor cx, cx
        xor dx, dx
        xor bx, bx
        xor bp, bp
        xor si, si
        xor di, di
        popa
        mov [r_popa], ax
        mov [r_popa+2], cx
        mov [r_popa+4], dx
        mov [r_popa+6], bx
        mov [r_popa+8], sp
        mov [r_popa+10], bp
        mov [r_popa+12], si
        mov [r_popa+14], di
        push 1234h
        push byte -5
        pop ax                  ; FFFBh
        pop bx                  ; 1234h
        imul cx, bx, 3          ; 369Ch
        imul dx, bx, 1000h      ; 1234000h: keeps 4000h, CF = OF = 1
        pushf
        pop si
        and si, 0801h
        mov [r_imulf], si
        mov si, 0F00Fh
        shl si, 4               ; 00F0h
        shr si, 4               ; 000Fh
        mov bp, 0ABCDh
        call frame0
        call frame2
        mov di, 5
        bound di, [limits]      ; 0 <= 5 <= 10: no exception
        hlt

frame0: enter 4, 0
        mov word [bp-2], 0BEEFh
        mov ax, [bp-2]
        mov [r_local], ax
        mov ax, [bp]            ; the caller's BP
        mov [r_oldbp], ax
        leave
        ret

frame2: enter 2, 2              ; pushes BP, copies one outer frame pointer, pushes the new one
        mov ax, bp
        sub ax, sp              ; 2 pointers + 2 bytes of locals = 6
        neg ax
        mov [r_depth], ax       ; FFFAh
        mov ax, 0FFFFh
        mov [r_depth+2], ax
        mov ax, 0FFFBh          ; leave AX as the test expects
        leave
        ret

limits: dw 0, 10

        times 300h - ($ - $$) db 0
r_popa:  times 8 dw 0           ; 1000:0400
r_imulf: dw 0                   ; 1000:0410
r_local: dw 0                   ; 1000:0412
r_oldbp: dw 0                   ; 1000:0414
r_depth: dw 0, 0                ; 1000:0416

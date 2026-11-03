; Real-mode protection on the 386: the faults a 386 raises where an 8086 would wrap or carry on, caught by handlers that
; count them and step over the faulting instruction (the pushed IP points at it): an offset above FFFFh and a word at
; FFFFh (#GP), a word at SS:FFFFh (#SS), UD2 and LOCK NOP (#UD), a 32-bit divide by zero (#DE), BOUND (#BR). A faulting
; load leaves its destination as it was. Then the vector table is moved with LIDT, an INT goes through the new table,
; and SIDT/LIDT put the old one back.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 386 486
; expect: ax=1234 bp=ffff sp=fffe
; expect-mem: 1000:0400 02 01 02 01 01 01 00 00 ff 03 00 00 00 00
        cpu 386
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        xor ax, ax
        mov fs, ax              ; FS = 0: the vector table
        mov word [fs:0Dh*4], gp_isr
        mov [fs:0Dh*4+2], cs
        mov word [fs:0Ch*4], ss_isr
        mov [fs:0Ch*4+2], cs
        mov word [fs:06h*4], ud_isr
        mov [fs:06h*4+2], cs
        mov word [fs:00h*4], de_isr
        mov [fs:00h*4+2], cs
        mov word [fs:05h*4], br_isr
        mov [fs:05h*4+2], cs
        mov ax, 1234h
        mov ebx, 10000h
        mov al, [ebx]           ; 67 8A 03: offset 10000h, #GP
        mov ax, [0FFFFh]        ; A1 FF FF: a word at the limit, #GP
        mov bp, 0FFFFh
        mov ax, [bp+0]          ; 8B 46 00: a word at SS:FFFFh, #SS
        db 0Fh, 0Bh             ; UD2: #UD
        db 0F0h, 90h            ; LOCK NOP: #UD
        push ax
        xor ebx, ebx
        xor edx, edx
        div ebx                 ; 66 F7 F3: #DE, pointing at the DIV
        mov eax, 20
        bound eax, [limits]     ; 66 62 06 lo hi: 20 is outside 0..10, #BR
        pop ax

        sidt [old_idtr]         ; move the vector table to 1000:0600 for one INT 50h
        xor ecx, ecx
        mov cx, cs
        shl ecx, 4
        add ecx, newidt
        mov [new_idtr+2], ecx
        mov word [newidt+50h*4], isr50
        mov [newidt+50h*4+2], cs
        lidt [new_idtr]
        int 50h
        lidt [old_idtr]
        hlt

gp_isr: inc byte [n_gp]
        jmp skip3
ss_isr: inc byte [n_ss]
        jmp skip3
de_isr: inc byte [n_de]
skip3:  push bp
        mov bp, sp
        add word [bp+2], 3      ; [bp+2] is the IP of the faulting instruction
        pop bp
        iret
ud_isr: inc byte [n_ud]
        push bp
        mov bp, sp
        add word [bp+2], 2
        pop bp
        iret
br_isr: inc byte [n_br]
        push bp
        mov bp, sp
        add word [bp+2], 5
        pop bp
        iret
isr50:  inc byte [n_50]
        iret

limits: dd 0, 10
new_idtr: dw 03FFh
        dd 0

        times 300h - ($ - $$) db 0
n_gp:   db 0                    ; 1000:0400
n_ss:   db 0
n_ud:   db 0
n_de:   db 0
n_br:   db 0
n_50:   db 0
        dw 0
old_idtr: dw 0                  ; 1000:0408: SIDT of the reset IDTR (limit 3FFh, base 0)
        dd 0
        times 600h - ($ - $$) db 0
newidt: times 400h db 0

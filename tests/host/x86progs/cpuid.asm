; Classic CPU identification, run on every model: FLAGS bits 12-15 (always 1 on the 8086 and V30, always 0 on a 286 in
; real mode), AAD with base 16 (the NEC chips always use 10), PUSH SP (the 8086 and V30 push the decremented value) and
; the shift count (the 8086 shifts 32 times, the 186-class chips mask the count to 0).
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286
; expect[8086]: ax=0000 dx=0002 si=0000
; expect[v30]: ax=0001 dx=0002 si=0001
; expect[286]: ax=0002 dx=0000 si=0001
        cpu 286
        bits 16
        org 100h

start:  pushf
        pop ax
        and ax, 0FFFh
        push ax
        popf
        pushf
        pop ax
        and ax, 0F000h
        cmp ax, 0F000h
        jne is286
        mov ax, 0101h
        db 0D5h, 10h            ; AAD 16: Intel 11h, NEC 0Bh
        cmp al, 11h
        je is8086
        mov bx, 1
        jmp done
is8086: mov bx, 0
        jmp done
is286:  mov bx, 2
done:   mov dx, sp
        push sp
        pop cx
        sub dx, cx
        mov si, 1
        mov cl, 32
        shl si, cl
        mov ax, bx
        hlt

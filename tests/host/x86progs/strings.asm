; String instructions: REP MOVSB forwards, REPE CMPSB, REPNE SCASB, REP STOSW, an overlapping REP MOVSB backwards (DF=1),
; LODSB with LOOP, and a CS: override on the source of REP MOVSW while DS points somewhere else.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286
; expect: ax=6f54 bp=0012 dx=0007 bx=0a0b cx=0000
; expect-mem: 1000:0400 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 54 6f 75 68
; expect-mem: 1000:0420 54 6f 54 6f 75 68 6f 75 20 6b 61 70 70 61
        cpu 8086
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        cld
        mov si, msg             ; 1. copy the message
        mov di, buf
        mov cx, msglen
        rep movsb
        mov si, buf             ; 2. first difference from msg2
        mov di, msg2
        mov cx, msglen
        repe cmpsb
        mov bp, si
        sub bp, buf + 1         ; bp = index of the mismatch (18)
        mov di, buf             ; 3. where is the 'k'?
        mov al, 'k'
        mov cx, msglen
        repne scasb
        mov dx, di
        sub dx, buf + 1         ; dx = 7
        mov di, fill            ; 4. eight words of 5A5Ah
        mov ax, 5A5Ah
        mov cx, 8
        rep stosw
        std                     ; 5. move buf two bytes up, overlapping, backwards
        mov si, buf + msglen - 1
        mov di, buf + msglen + 1
        mov cx, msglen
        rep movsb
        cld
        mov si, buf             ; 6. byte sum of the moved buffer
        mov cx, msglen + 2
        xor bx, bx
        xor ah, ah
sum:    lodsb
        add bx, ax
        loop sum
        mov ax, 2000h           ; 7. DS points away; the source comes from CS by override
        mov ds, ax
        mov si, msg
        mov di, copy2
        mov cx, 2
        cs rep movsw
        mov ax, cs
        mov ds, ax
        mov ax, [copy2]         ; 'To'
        hlt

msg:    db "Touhou kappa string test"
msglen  equ $ - msg
msg2:   db "Touhou kappa strinG test"

        times 300h - ($ - $$) db 0
fill:   times 8 dw 0            ; 1000:0400
copy2:  dw 0, 0                 ; 1000:0410
        times 320h - ($ - $$) db 0
buf:    times msglen + 2 db 0   ; 1000:0420

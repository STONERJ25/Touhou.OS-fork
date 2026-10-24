; The smallest PC-98 boot sector of the TouhouOS emulator tests: copies one line into text VRAM and stops.
; tests/host/test_pc98.py carries the same program as hand-assembled bytes (TINY_BOOT) so the boot demo runs without
; nasm; when nasm is there, the test also checks that this source assembles to exactly those bytes.
        cpu 8086
        bits 16
        org 0

start:  push cs
        pop ds
        mov ax, 0A000h          ; text VRAM
        mov es, ax
        mov si, msg
        xor di, di
        cld
        xor ah, ah
next:   lodsb
        or al, al
        jz done
        stosw                   ; character code, then 00h: one cell
        jmp next
done:   cli
        hlt
msg:    db "TOUHOU OS: SHRINE BOOT OK", 0

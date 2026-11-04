; diskwrite.asm: a boot sector that writes to its own floppy, for the tests of the disk overlay (tests/host/test_pc98app.py).
; It puts a line of text into a 1024-byte buffer and writes it to cylinder 0, head 0, sector 5 of drive 90h with INT 1Bh AH=05h; the
; status the BIOS answered goes to 0000:0600 and the program spins. The tests then look at the in-memory copy of the image (changed
; blocks), at the file (never touched) and at the file "Save changes as" makes (the new bytes in exactly that sector).
        cpu 8086
        bits 16
        org 0

BUF     equ 0400h                       ; just past this sector: RAM that the boot sector's load did not touch (zeros)

start:  push cs
        pop ds
        push cs
        pop es
        cld
        mov si, msg
        mov di, BUF
        mov cx, msglen
        rep movsb
        mov ax, 0590h                   ; write, drive 90h
        mov bx, 1024
        mov cx, 0300h                   ; CL = cylinder 0, CH = size code 3 (1024 bytes)
        mov dx, 0005h                   ; DH = head 0, DL = sector 5
        mov bp, BUF                     ; ES:BP = the buffer
        int 1Bh
        mov bl, ah
        xor ax, ax
        mov ds, ax
        mov [0600h], bl
        mov word [0602h], 4B4Fh         ; "OK": the program got this far
.spin:  jmp .spin

msg:    db "WRITTEN BY THE GUEST", 0
msglen  equ $ - msg

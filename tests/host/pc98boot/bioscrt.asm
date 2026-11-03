; bioscrt.asm: a boot sector that exercises the HLE BIOS's CRT functions, INT 18h AH=0Ah, 0Bh, 0Ch, 0Dh, 10h, 11h,
; 12h, 13h, 16h, 17h, 18h, 40h, 42h and a function the BIOS does not have (user/lib/pc98/src/pc98bios.c,
; docs/PC98_BIOS.md). Written for tests/host/test_pc98bios.py, which checks 0000:0600 and the machine state that
; pc98run --state prints at the end: text and graphics on, the cursor at row 2 column 5, steady, the beeper off,
; graphics mode C0h, and every text cell a green "*".
;
;   nasm -f bin -o bioscrt.bin bioscrt.asm

        bits 16
        org 0

start:
        mov di, 0x600
        mov ax, 0x0a03                  ; CRT mode: 20 lines, 40 columns ...
        int 0x18
        mov ah, 0x0b
        int 0x18
        mov [di], al                    ; 600: 03
        mov ax, 0x0a00                  ; ... and back to 25 lines of 80
        int 0x18
        mov ah, 0x0b
        int 0x18
        mov [di+1], al                  ; 601: 00
        mov ah, 0x16                    ; fill text VRAM: "*", attribute 81h (green, visible)
        mov dx, 0x812a
        int 0x18
        mov ah, 0x13                    ; cursor: row 2, column 5 (DX = byte offset in text VRAM)
        mov dx, (2 * 80 + 5) * 2
        int 0x18
        mov ah, 0x12                    ; cursor off and on again
        int 0x18
        mov ah, 0x11
        int 0x18
        mov ax, 0x1001                  ; cursor steady
        int 0x18
        mov ah, 0x17                    ; beeper on: 8255 port C bit 3 low
        int 0x18
        in al, 0x35
        mov [di+2], al                  ; 602
        mov ah, 0x18                    ; beeper off
        int 0x18
        in al, 0x35
        mov [di+3], al                  ; 603
        mov ah, 0x0d                    ; text display off, then on
        int 0x18
        mov ah, 0x0c
        int 0x18
        mov ah, 0x40                    ; graphics on, 640 x 400
        int 0x18
        mov ah, 0x42
        mov ch, 0xc0
        int 0x18
        mov ah, 0x7f                    ; a function the BIOS does not have: CF set
        clc
        int 0x18
        mov al, 0
        adc al, 0
        mov [di+4], al                  ; 604: 1
        cli
        hlt

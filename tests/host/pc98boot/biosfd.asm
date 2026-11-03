; biosfd.asm: the boot sector of a 2HD floppy (1024-byte sectors) for the HLE BIOS (user/lib/pc98/src/pc98bios.c,
; docs/PC98_BIOS.md). The BIOS loads the whole first sector to 1FC0:0000 and enters it with AL = DA/UA (90h for the
; first 1 MB drive). Written for tests/host/test_pc98bios.py, which builds a D88 image whose sectors hold known
; patterns. Results at 0000:0600: 600 AL at entry, 601 [0584h], 602 AH and CF of a two-sector multi-track read from
; C0 H0 R8 (R8, then R1 of head 1) to 2000:0000, 604 the same for a sense, 606 for a sector that does not exist (R9),
; 608 the word at the end of this sector (A55Ah: the second half of the sector was loaded too).
;
;   nasm -f bin -o biosfd.bin biosfd.asm

        bits 16
        org 0

%macro RES 0
        mov [di], ah
        mov al, 0
        adc al, 0
        mov [di+1], al
        add di, 2
%endmacro

start:
        mov [0x600], al
        mov al, [0x584]
        mov [0x601], al
        mov di, 0x602
        mov ax, 0x2000
        mov es, ax
        xor bp, bp
        mov ax, 0xd690                  ; MT, MFM, seek; read data; unit 90h
        mov bx, 2048
        mov cx, 0x0300                  ; CH = 3: 1024-byte sectors; CL = cylinder 0
        mov dx, 0x0008                  ; DH = head 0, DL = sector 8
        int 0x1b
        RES                             ; 602
        mov ax, 0x0490                  ; sense
        int 0x1b
        RES                             ; 604
        mov ax, 0xd690                  ; R9: not on the track
        mov bx, 1024
        mov cx, 0x0300
        mov dx, 0x0009
        int 0x1b
        RES                             ; 606
        mov ax, [cs:0x3fe]
        mov [0x608], ax
        cli
        hlt

        times 0x3fe - ($ - $$) db 0
        dw 0xa55a

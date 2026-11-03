; biosdisk.asm: a boot sector that exercises the HLE BIOS's INT 1Bh on a hard disk (user/lib/pc98/src/pc98bios.c,
; docs/PC98_BIOS.md). Written for tests/host/test_pc98bios.py, which builds the disk: an HDI of 20 cylinders, 4 heads,
; 17 sectors of 512 bytes, where sector n starts with the word n. Results go to 0000:0600 (byte pairs AH, CF and the
; words noted below); the data read lands at 2000:0000. The program ends by returning to the BIOS (RETF) with
; CX = B007h, which the BIOS reports as "the boot program returned".
;
;   nasm -f bin -o biosdisk.bin biosdisk.asm

        bits 16
        org 0

%macro RES 0                            ; AH and CF of the last call, two bytes at [DI]
        mov [di], ah
        mov al, 0
        adc al, 0
        mov [di+1], al
        add di, 2
%endmacro

start:
        mov [0x600], al                 ; AL at entry: the DA/UA
        mov bl, [0x584]                 ; DS = 0 on entry: the work area is at DS:0400
        mov [0x601], bl
        mov [0x602], ds
        mov [0x604], ss
        mov [0x606], sp
        mov [0x608], cs
        mov di, 0x610

        mov ax, 0x8480                  ; new sense: sector size, cylinders, heads and sectors
        int 0x1b
        RES                             ; 610
        mov [di], bx                    ; 612
        mov [di+2], cx                  ; 614
        mov [di+4], dx                  ; 616
        add di, 6
        mov ax, 0x0480                  ; sense
        int 0x1b
        RES                             ; 618
        mov ax, 0x8e80                  ; the call the IPL makes before using a 512-byte-sector disk
        int 0x1b
        RES                             ; 61A
        mov ax, 0x0380                  ; initialise
        int 0x1b
        RES                             ; 61C
        mov ax, 0x0780                  ; recalibrate
        int 0x1b
        RES                             ; 61E

        mov ax, 0x2000                  ; three sectors from C0 H0 S15: S15, S16, then H1 S0 (sectors 15, 16, 17)
        mov es, ax
        xor bp, bp
        mov ax, 0x0680
        mov bx, 3 * 512
        xor cx, cx
        mov dx, 0x000f
        int 0x1b
        RES                             ; 620
        mov bp, 0x600                   ; across the cylinder: C0 H3 S16, C1 H0 S0 (67, 68) to 2000:0600
        mov ax, 0x0680
        mov bx, 2 * 512
        xor cx, cx
        mov dx, 0x0310
        int 0x1b
        RES                             ; 622
        mov bp, 0xa00                   ; relative sector numbers (DA/UA 00h): DX:CX = 300, two sectors, to 2000:0A00
        mov ax, 0x0600
        mov bx, 1024
        mov cx, 300
        xor dx, dx
        int 0x1b
        RES                             ; 624

        mov ax, 0x3000                  ; write C1 H0 S0 (sector 68) from 3000:0000 (512 bytes of 5Ah) ...
        mov es, ax
        push di
        xor di, di
        mov cx, 256
        mov ax, 0x5a5a
        cld
        rep stosw
        pop di
        xor bp, bp
        mov ax, 0x0580
        mov bx, 512
        mov cx, 1
        xor dx, dx
        int 0x1b
        RES                             ; 626
        mov ax, 0x2000                  ; ... and read it back to 2000:0E00
        mov es, ax
        mov bp, 0xe00
        mov ax, 0x0680
        mov bx, 512
        mov cx, 1
        xor dx, dx
        int 0x1b
        RES                             ; 628
        mov ax, 0x0180                  ; verify four sectors at C0 H0 S0
        mov bx, 2048
        xor cx, cx
        xor dx, dx
        int 0x1b
        RES                             ; 62A

        mov ax, 0x0681                  ; errors: a unit that is not there,
        mov bx, 512
        xor cx, cx
        xor dx, dx
        int 0x1b
        RES                             ; 62C
        mov ax, 0x0680                  ; a cylinder beyond the disk,
        mov cx, 999
        int 0x1b
        RES                             ; 62E
        mov ax, 0x0d80                  ; a command the BIOS does not have (format)
        int 0x1b
        RES                             ; 630

        mov si, 0x1234                  ; a read keeps the other registers: C2 H1 S2 (sector 155) to 2000:1000
        mov ax, 0x0680
        mov bx, 512
        mov cx, 2
        mov dx, 0x0102
        mov bp, 0x1000
        int 0x1b
        mov [di], bx                    ; 632
        mov [di+2], cx                  ; 634
        mov [di+4], dx                  ; 636
        mov [di+6], bp                  ; 638
        mov [di+8], si                  ; 63A
        mov [di+10], es                 ; 63C
        mov ax, 0x8400                  ; new sense with relative addressing: the same drive
        int 0x1b
        mov [di+12], bx                 ; 63E
        add di, 14
        mov ax, 0x0f80                  ; what the STOP-key handler of IO.SYS calls for every hard disk
        int 0x1b
        RES                             ; 640

        mov cx, 0xb007
        retf                            ; back to the BIOS: "the boot program returned"

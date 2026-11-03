; biosmisc.asm: a boot sector that looks at what the HLE BIOS leaves behind (the work area, the vectors, the memory
; switches; user/lib/pc98/src/pc98bios.c, docs/PC98_BIOS.md), makes a call to a BIOS service that does not exist,
; divides by zero and finally calls into the ROM page where there is no entry point. Written for
; tests/host/test_pc98bios.py.
;
; Results at 0000:0600: 600 [0500h], 601 [0501h], 602 the word [055Ch], 604 [0584h], 606 and 608 the keyboard buffer's
; head and tail pointers, 60A [0528h] (keys in the buffer), 60B [0401h], 60C and 60E INT 18h's vector (offset,
; segment), 610 INT 1Bh's segment, 612 .. 619 memory switches 1 to 8, 61A CF after INT 19h, 61B a progress mark:
; 11h before the divide, 22h after it (an 8086 or V30 continues after a divide error; a 286 returns to the divide
; itself, faults again and again, and the BIOS stops the CPU), never 33h (the far call stops the CPU).
;
;   nasm -f bin -o biosmisc.bin biosmisc.asm

        bits 16
        org 0

start:
        xor ax, ax
        mov es, ax
        mov al, [0x500]
        mov [0x600], al
        mov al, [0x501]
        mov [0x601], al
        mov ax, [0x55c]
        mov [0x602], ax
        mov al, [0x584]
        mov [0x604], al
        mov ax, [0x524]
        mov [0x606], ax
        mov ax, [0x526]
        mov [0x608], ax
        mov al, [0x528]
        mov [0x60a], al
        mov al, [0x401]
        mov [0x60b], al
        mov ax, [0x18 * 4]
        mov [0x60c], ax
        mov ax, [0x18 * 4 + 2]
        mov [0x60e], ax
        mov ax, [0x1b * 4 + 2]
        mov [0x610], ax

        push ds                         ; memory switches: text attribute RAM A3FE2, A3FE6, ... A3FFE
        mov ax, 0xa000
        mov ds, ax
        mov si, 0x3fe2
        mov di, 0x612
        mov cx, 8
.sw:    mov al, [si]
        mov [es:di], al
        add si, 4
        inc di
        loop .sw
        pop ds

        mov ah, 0x00                    ; RS-232C initialise: no such BIOS here, CF set
        clc
        int 0x19
        mov al, 0
        adc al, 0
        mov [0x61a], al

        mov byte [0x61b], 0x11
        xor cx, cx
        div cl                          ; divide error: INT 00h
        mov byte [0x61b], 0x22
        call 0xfd80:0x1234              ; nothing there: the BIOS stops the CPU
        mov byte [0x61b], 0x33
        cli
        hlt

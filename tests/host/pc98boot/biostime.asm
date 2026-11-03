; biostime.asm: a boot sector that exercises the HLE BIOS's INT 1Ch: AH=00h (read the calendar), AH=01h (set it) and
; AH=02h (the interval timer, which calls the routine in INT 07h's vector after CX ticks of 10 ms; user/lib/pc98/src/
; pc98bios.c, docs/PC98_BIOS.md). Written for tests/host/test_pc98bios.py (pc98run --clock 261102120000).
;
; Results at 0000:0600: the calendar as it was at power-on (6 BCD bytes: year, month << 4 | weekday, day, hour, minute,
; second), at 0608 the calendar read right after setting it to 2001-02-03 04:05:06, at 0610 how many times HLT woke up
; before the timer routine ran (word), at 0612 how many times it ran, at 0613 the master interrupt mask afterwards.
;
;   nasm -f bin -o biostime.bin biostime.asm

        bits 16
        org 0

start:
        push ds                         ; DS = 0: ES:BX = 0000:0600
        pop es
        mov bx, 0x600
        mov ah, 0x00
        int 0x1c                        ; 600 .. 605
        push cs
        pop es
        mov bx, settime
        mov ah, 0x01
        int 0x1c
        push ds
        pop es
        mov bx, 0x608
        mov ah, 0x00
        int 0x1c                        ; 608 .. 60D

        push cs                         ; interval timer: three ticks, then CS:tick
        pop es
        mov bx, tick
        mov cx, 3
        mov ah, 0x02
        int 0x1c
        sti
        xor si, si
.halt:  hlt
        inc si
        cmp byte [cs:count], 0
        jnz .fired
        cmp si, 50
        jb .halt
.fired: mov [0x610], si
        mov al, [cs:count]
        mov [0x612], al
        in al, 0x02
        mov [0x613], al
        cli
        hlt

tick:   inc byte [cs:count]             ; called through INT 07h from the BIOS's timer interrupt
        iret

count:  db 0
settime: db 0x01, 0x20, 0x03, 0x04, 0x05, 0x06     ; 2001, February (month 2 in the high nibble), 3rd, 04:05:06

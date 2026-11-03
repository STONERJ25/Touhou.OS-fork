; A PC-98 boot sector, written from scratch for the TouhouOS emulator tests (tests/host/test_pc98.py). No BIOS calls:
; everything goes straight to the hardware the machine skeleton models.
;
; The BIOS (here: pc98_boot) loads it to 1FC0:0000 and jumps there with AL = the boot device (DA/UA) and interrupts off.
; It then
;   1. writes a banner into text VRAM (A000h) and colours it through the attribute plane (A200h),
;   2. prints the boot device in hex,
;   3. waits for three VSYNCs by polling the text GDC's status port (60h, bit 5),
;   4. programs the 8259 and the 8253 (counter 0, mode 2), installs an INT 08h handler and sleeps in HLT until five
;      timer interrupts have arrived,
;   5. sets the beeper to 2 kHz (PIT counter 1 at 3FDBh) and switches it on and off (port 37h, bit 3 of port C),
;   6. reads the text GDC's cursor back after setting it (CSRW 49h, CSRR E0h),
;   7. prints a line per step and stops with CLI; HLT.
        cpu 8086
        bits 16
        org 0

TVRAM   equ 0A000h
ATTR    equ 0A200h
ROW     equ 160                 ; bytes per text row (80 cells of one word)

start:  mov [cs:bootdev], al
        cld
        mov ax, cs
        mov ds, ax
        mov ax, TVRAM
        mov es, ax

        xor di, di              ; row 0: the banner, in yellow (G+R, visible)
        mov si, banner
        call puts
        mov ax, ATTR
        mov es, ax
        xor di, di
        mov ax, 00C1h
        mov cx, 80
        rep stosw
        mov ax, TVRAM
        mov es, ax

        mov di, 1 * ROW         ; row 1: boot device
        mov si, devmsg
        call puts
        mov al, [bootdev]
        call puthex

        mov cx, 3               ; row 2: three VSYNCs
vwait:  in al, 60h
        test al, 20h
        jnz vwait               ; leave the current VSYNC
vwait2: in al, 60h
        test al, 20h
        jz vwait2               ; wait for the next one to begin
        loop vwait
        mov di, 2 * ROW
        mov si, vsmsg
        call puts

        xor ax, ax              ; row 3: timer interrupts
        mov es, ax
        mov word [es:08h * 4], timer
        mov [es:08h * 4 + 2], cs
        mov al, 11h             ; ICW1: edge, cascade, ICW4 follows
        out 00h, al
        mov al, 08h             ; ICW2: vectors 08h-0Fh
        out 02h, al
        mov al, 80h             ; ICW3: a slave on IR7
        out 02h, al
        mov al, 1Dh             ; ICW4: special fully nested, buffered master, 8086 mode
        out 02h, al
        mov al, 0FEh            ; OCW1: only IR0
        out 02h, al
        mov al, 34h             ; counter 0, LSB then MSB, mode 2
        out 77h, al
        mov ax, 24576           ; 100 Hz on a 2.4576 MHz timer clock
        out 71h, al
        mov al, ah
        out 71h, al
        sti
sleep:  hlt
        cmp word [ticks], 5
        jb sleep
        cli
        mov ax, TVRAM
        mov es, ax
        mov di, 3 * ROW
        mov si, tmmsg
        call puts
        mov al, [ticks]
        call puthex

        mov al, 76h             ; row 4: beeper: counter 1, LSB then MSB, mode 3
        out 77h, al
        mov dx, 3FDBh
        mov ax, 1229            ; about 2 kHz
        out dx, al
        mov al, ah
        out dx, al
        mov al, 06h             ; port C bit 3 = 0: buzzer on
        out 37h, al
        mov al, 07h             ; bit 3 = 1: off
        out 37h, al
        mov di, 4 * ROW
        mov si, bpmsg
        call puts

        mov al, 49h             ; row 5: GDC cursor write and read back
        out 62h, al
        mov al, 34h
        out 60h, al
        mov al, 12h
        out 60h, al
        mov al, 0E0h
        out 62h, al
        in al, 62h
        mov bl, al
        in al, 62h
        mov bh, al
        mov di, 5 * ROW
        mov si, gdcmsg
        call puts
        mov al, bh
        call puthex
        mov al, bl
        call puthex

        mov di, 7 * ROW
        mov si, donemsg
        call puts
        cli
        hlt

timer:  push ax
        inc word [cs:ticks]
        mov al, 20h             ; non-specific EOI
        out 00h, al
        pop ax
        iret

; puts: DS:SI = NUL-terminated text, ES:DI = cell; writes one word per character (code, 0)
puts:   lodsb
        or al, al
        jz .done
        xor ah, ah
        stosw
        jmp puts
.done:  ret

; puthex: AL as two hex digits at ES:DI
puthex: push ax
        mov cl, 4
        shr al, cl
        call .nib
        pop ax
        and al, 0Fh
.nib:   add al, '0'
        cmp al, '9'
        jbe .out
        add al, 7
.out:   xor ah, ah
        stosw
        ret

bootdev: db 0
ticks:   dw 0
banner:  db "Hakurei Shrine boot sector, no ROM inside", 0
devmsg:  db "boot device (DA/UA): ", 0
vsmsg:   db "three VSYNCs seen on the text GDC", 0
tmmsg:   db "timer interrupts counted: ", 0
bpmsg:   db "the beeper sang at 2 kHz", 0
gdcmsg:  db "GDC cursor read back: ", 0
donemsg: db "The barrier holds. Halting.", 0

        times 510 - ($ - $$) db 0     ; fits a 512-byte hard disk sector too
        db 55h, 0AAh

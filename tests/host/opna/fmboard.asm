; A PC-98 boot sector that finds an FM sound board and lives on its timer interrupts, written from scratch for the
; TouhouOS emulator tests (tests/host/test_opna.py, docs/PC98_SOUND.md). No BIOS calls: it talks to the board the way
; a music driver does, through the chip's ports, and to the interrupt controllers directly.
;
; The BIOS (here: pc98_boot) loads it to 1FC0:0000 and jumps there with interrupts off. It then
;   1. writes an SSG register and reads it back (an OPN or OPNA is there),
;   2. reads register FFh (01h: a YM2608), and on a -86 board switches the second register bank off and on through
;      port A460h, reading status 1 both times (off: the port is not decoded and reads FFh),
;   3. reads the interrupt jumper from SSG port A bits 7-6 and installs a handler at that interrupt's vector,
;   4. programs both 8259s, starts timer B (NB = 200: 56 x 16 samples = 16.2 ms) with its flag enabled, keys a 440 Hz
;      FM note on, and sleeps in HLT until eight timer B interrupts have come; the handler resets the flag each time,
;   5. stops the timer and the note and prints a line per step, then CLI; HLT.
        cpu 8086
        bits 16
        org 0

TVRAM   equ 0A000h
ROW     equ 160
OPN_A   equ 0188h                       ; address / status 0
OPN_D   equ 018Ah                       ; data
OPNA_A1 equ 018Ch                       ; second bank: address / status 1
BOARD   equ 0A460h                      ; -86 board: bit 0 = OPNA mode (the second bank answers)

start:  cld
        mov ax, cs
        mov ds, ax
        mov ax, TVRAM
        mov es, ax

        mov ax, 0BAAh                   ; 1. R11 = AAh, read it back
        call opn_write
        mov al, 0Bh
        call opn_read
        cmp al, 0AAh
        je .found
        mov si, nomsg
        xor di, di
        call puts
        cli
        hlt
.found: mov al, 0FFh                    ; 2. the ID register
        call opn_read
        mov [chipid], al
        cmp al, 1
        jne .jumper
        mov dx, BOARD
        in al, dx
        and al, 0FCh                    ; OPNA mode off
        out dx, al
        mov dx, OPNA_A1
        in al, dx
        mov [bank_off], al
        mov dx, BOARD
        in al, dx
        and al, 0FCh
        or al, 1                        ; on
        out dx, al
        mov dx, OPNA_A1
        in al, dx
        and al, 7Fh                     ; (BUSY may still be up)
        mov [bank_on], al

.jumper:
        mov ax, 07BFh                   ; 3. R7: tones and noise off, port A in, port B out
        call opn_write
        mov al, 0Eh
        call opn_read
        mov cl, 6
        shr al, cl
        mov bl, al
        xor bh, bh
        mov al, [irq_of + bx]
        mov [irq], al
        mov al, [vec_of + bx]
        mov [vector], al

        xor ax, ax                      ; the handler at the jumper's vector
        mov es, ax
        mov bl, [vector]
        xor bh, bh
        shl bx, 1
        shl bx, 1
        mov word [es:bx], handler
        mov [es:bx + 2], cs

        mov al, 11h                     ; 4. master 8259: edge, cascade, ICW4; vectors 08h-0Fh; slave on IR7
        out 00h, al
        mov al, 08h
        out 02h, al
        mov al, 80h
        out 02h, al
        mov al, 1Dh
        out 02h, al
        mov al, 11h                     ; slave: vectors 10h-17h, cascade identity 7
        out 08h, al
        mov al, 10h
        out 0Ah, al
        mov al, 07h
        out 0Ah, al
        mov al, 09h
        out 0Ah, al
        mov cl, [irq]                   ; unmask the board's line (and the cascade for a slave line)
        cmp cl, 8
        jb .master
        sub cl, 8
        mov al, 1
        shl al, cl
        not al
        out 0Ah, al
        mov al, 7Fh
        out 02h, al
        jmp .timer
.master:
        mov al, 1
        shl al, cl
        not al
        out 02h, al
        mov al, 0FFh
        out 0Ah, al

.timer: mov ax, 2983h                   ; YM2608: six channels, timer interrupts enabled (harmless on a YM2203)
        call opn_write
        mov si, note                    ; a sine on channel 1: algorithm 7, S4 alone
.patch: lodsw
        or ax, ax
        jz .go
        call opn_write
        jmp .patch
.go:    mov ax, 26C8h                   ; NB = 200
        call opn_write
        mov ax, 272Ah                   ; reset B, enable B, load B
        call opn_write
        sti
.sleep: hlt
        cmp word [ticks], 8
        jb .sleep
        cli
        mov ax, 2730h                   ; 5. timers stopped, flags reset
        call opn_write
        mov ax, 2800h                   ; key off
        call opn_write

        mov ax, TVRAM
        mov es, ax
        xor di, di
        mov si, banner
        call puts
        mov di, 1 * ROW
        mov si, idmsg
        call puts
        mov al, [chipid]
        call puthex
        mov di, 2 * ROW
        mov si, bankmsg
        call puts
        mov al, [bank_off]
        call puthex
        mov si, bankmsg2
        call puts
        mov al, [bank_on]
        call puthex
        mov di, 3 * ROW
        mov si, irqmsg
        call puts
        mov al, [irq]
        call puthex
        mov si, vecmsg
        call puts
        mov al, [vector]
        call puthex
        mov di, 4 * ROW
        mov si, tbmsg
        call puts
        mov al, [ticks]
        call puthex
        mov di, 5 * ROW
        mov si, donemsg
        call puts
        cli
        hlt

; The board's interrupt: timer B as long as its flag is up (reset, count), then end-of-interrupt to the 8259s.
handler:
        push ax
        push dx
.again: mov dx, OPN_A
        in al, dx
        test al, 02h
        jz .eoi
        mov ax, 272Ah
        call opn_write
        inc word [cs:ticks]
        jmp .again
.eoi:   mov al, 20h
        cmp byte [cs:irq], 8
        jb .m
        out 08h, al
.m:     out 00h, al
        pop dx
        pop ax
        iret

; opn_write: AH = register, AL = value (bank 0), after waiting for BUSY to clear
opn_write:
        push dx
        push ax
        mov dx, OPN_A
.busy:  in al, dx
        test al, 80h
        jnz .busy
        pop ax
        push ax
        xchg al, ah
        out dx, al
        xchg al, ah
        mov dx, OPN_D
        out dx, al
        pop ax
        pop dx
        ret

; opn_read: AL = register -> AL = what the data port reads
opn_read:
        push dx
        push ax
        mov dx, OPN_A
.busy:  in al, dx
        test al, 80h
        jnz .busy
        pop ax
        out dx, al
        mov dx, OPN_D
        in al, dx
        pop dx
        ret

puts:   lodsb
        or al, al
        jz .done
        xor ah, ah
        stosw
        jmp puts
.done:  ret

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

; port A bits 7-6 -> the interrupt the jumper selects, and its vector (master base 08h, slave base 10h)
irq_of: db 3, 13, 10, 12
vec_of: db 0Bh, 15h, 12h, 14h
; register, value pairs (AH, AL) for the note; 0 ends the list
note:   dw 0B007h                       ; channel 1: algorithm 7, no feedback
        dw 03C01h                       ; S4: MUL 1
        dw 04C00h                       ; S4: TL 0
        dw 05C1Fh                       ; S4: AR 31
        dw 06C00h, 07C00h, 08C0Fh       ; S4: no decay, release 15
        dw 0407Fh, 0447Fh, 0487Fh       ; S1-S3 silent
        dw 0A424h, 0A010h               ; block 4, F-number 410h: 440 Hz
        dw 02880h                       ; key on S4
        dw 0

chipid:   db 0
bank_off: db 0
bank_on:  db 0
irq:      db 0
vector:   db 0
ticks:    dw 0
banner:   db "FM board test, no ROM inside", 0
nomsg:    db "no FM board", 0
idmsg:    db "register FFh: ", 0
bankmsg:  db "second bank status, OPNA mode off: ", 0
bankmsg2: db ", on: ", 0
irqmsg:   db "interrupt: IRQ ", 0
vecmsg:   db ", vector ", 0
tbmsg:    db "timer B interrupts: ", 0
donemsg:  db "Halting.", 0

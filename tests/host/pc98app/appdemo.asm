; appdemo.asm: the boot sector that the application's QEMU tests (tests/case_pc98app.py) and the host tests run. Written from scratch for them.
; It uses the HLE BIOS for the keyboard state and the disk and the hardware directly for everything else, and does five things:
;
;   1. a picture that is known to the pixel: the graphics display is switched on (INT 18h AH=40h) and eight vertical bars of 80 pixels, one
;      for each colour of the 8-colour palette (blue = bit 0, red = bit 1, green = bit 2 of the colour number), fill the upper half
;      (rows 0-199) of the screen, and the text layer says "PC98APP DEMO" at row 20 (the character generator is whatever the application has
;      loaded: the built-in ASCII font without a ROM);
;   2. a box of 64 x 32 pixels at (16, 220): red while the Z key (PC-98 key 29h) is up, green while it is down, read from the BIOS's key status table;
;   3. the CRT's VSYNC interrupt counts frames, and the count is shown in hex at text row 22 (a game in miniature: it stops when the machine is paused
;      and starts again from 0000 when the machine is reset);
;   4. the buzzer sounds 2 kHz (PIT counter 1, port 37h) for the first 120 frames and stops;
;   5. the W key (11h) writes the sector C0 H0 R5 of drive 90h with the text "WRITTEN BY THE DEMO" (INT 1Bh AH=05h) and shows the BIOS's status in hex at row 24.
;
;   nasm -f bin -o appdemo.bin appdemo.asm        (it must stay under 1024 bytes: it is the whole boot sector of a 1024-byte-sector floppy)
        cpu 8086
        bits 16
        org 0

FRAMES  equ 0600h                       ; word: frames counted by the interrupt
BEEPING equ 0602h                       ; word: 1 while the buzzer is on
WDOWN   equ 0604h                       ; word: the W key was down at the last look (to act once per press)
TVRAM   equ 0A000h
ROW     equ 160                         ; bytes per text row
BUF     equ 0400h                       ; just past this sector (zeros): the buffer written to the disk

start:  cli
        cld
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov [FRAMES], ax
        mov [WDOWN], ax
        mov word [0x28], isr            ; IRQ 2 (the CRT's VSYNC) is INT 0Ah
        mov [0x2A], cs
        mov ah, 40h                     ; graphics display on
        int 18h

        ; ---- the bars: plane B at A800, R at B000, G at B800; bar k (0..7) is bytes 10k..10k+9 of each of rows 0..199
        mov bx, 0A800h                  ; first plane: bit 0 of the colour number
        mov dl, 1
planes: mov es, bx
        xor di, di
        mov cx, 200
rows:   push cx
        xor si, si                      ; si = bar number
bars:   mov ax, si
        test al, dl
        mov al, 0FFh
        jnz fill
        xor al, al
fill:   mov cx, 10
        rep stosb
        inc si
        cmp si, 8
        jb bars
        pop cx
        loop rows
        add bx, 0800h                   ; B000, then B800
        shl dl, 1
        cmp dl, 8
        jb planes

        ; ---- the text
        mov ax, TVRAM
        mov es, ax
        mov si, banner
        mov di, 20 * ROW
        push cs
        pop ds
        call puts
        mov si, hint
        mov di, 23 * ROW
        call puts
        xor ax, ax
        mov ds, ax

        ; ---- the buzzer: counter 1, mode 3, 2 kHz (1996800 / 998)
        mov al, 76h
        out 77h, al
        mov ax, 998
        out 73h, al
        mov al, ah
        out 73h, al
        mov al, 06h                     ; port C bit 3 low: on
        out 37h, al
        mov word [BEEPING], 1

        ; ---- the interrupt: unmask IRQ 2 on the master, arm the first CRT interrupt
        in al, 02h
        and al, 0FBh
        out 02h, al
        xor al, al
        out 64h, al
        sti

main:   hlt                             ; one frame per interrupt
        cli
        mov ax, [FRAMES]
        sti
        ; the frame counter, four hex digits at row 22
        mov bx, TVRAM
        mov es, bx
        mov di, 22 * ROW
        mov cx, 4
        mov dx, ax
digits: rol dx, 1
        rol dx, 1
        rol dx, 1
        rol dx, 1
        mov al, dl
        and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe dig
        add al, 7
dig:    mov ah, 0
        mov [es:di], ax
        add di, 2
        loop digits
        ; the buzzer stops after 120 frames
        cmp word [BEEPING], 0
        je nobeep
        cmp word [FRAMES], 120
        jb nobeep
        mov al, 07h                     ; port C bit 3 high: off
        out 37h, al
        mov word [BEEPING], 0
nobeep:
        ; the Z box: key status group 5 (keys 28h-2Fh), bit 1 = key 29h; red while up, green while down
        mov al, [052Ah + 5]
        and al, 02h
        mov bx, 0B000h                  ; red plane
        mov dx, 0B800h                  ; green plane
        jz up
        xchg bx, dx                     ; down: the box is green, the red plane gets zeros
up:     mov al, 0FFh                    ; bx plane gets ones, dx plane zeros
        call box
        ; the W key: group 2 (keys 10h-17h), bit 1 = key 11h
        mov al, [052Ah + 2]
        and al, 02h
        jz wup
        cmp word [WDOWN], 0
        jne main
        mov word [WDOWN], 1
        push cs
        pop es
        mov si, wtext
        mov di, BUF
        push cs
        pop ds
        mov cx, wlen
        cld
        rep movsb
        mov ax, 0590h                   ; write, drive 90h
        mov bx, 1024
        mov cx, 0300h                   ; cylinder 0, 1024-byte sectors
        mov dx, 0005h                   ; head 0, sector 5
        mov bp, BUF
        int 1Bh
        mov dl, ah
        xor ax, ax
        mov ds, ax
        mov ax, TVRAM
        mov es, ax
        mov al, dl                      ; the status as two hex digits at row 24, column 0
        mov cl, 4
        shr al, cl
        call hexdigit
        mov [es:24 * ROW], ax
        mov al, dl
        and al, 0Fh
        call hexdigit
        mov [es:24 * ROW + 2], ax
        jmp main
wup:    mov word [WDOWN], 0
        jmp main

; hexdigit: AL (0-15) to the character code in AX (AH = 0)
hexdigit:
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov ah, 0
        ret

; box: fills the box rows 220-251, bytes 2-9 of the plane in BX with AL and of the plane in DX with zeros
box:    push ax
        mov cx, 32
        mov di, 220 * 80 + 2
.r:     push cx
        mov es, bx
        mov cx, 8
        push di
        rep stosb
        pop di
        mov es, dx
        xor al, al
        mov cx, 8
        push di
        rep stosb
        pop di
        mov al, 0FFh
        add di, 80
        pop cx
        loop .r
        pop ax
        ret

; puts: DS:SI text (NUL ends it) to the text layer at ES:DI
puts:   lodsb
        or al, al
        jz .e
        mov ah, 0
        stosw
        jmp puts
.e:     ret

; the interrupt: count, re-arm, end of interrupt
isr:    push ax
        push ds
        xor ax, ax
        mov ds, ax
        inc word [FRAMES]
        xor al, al
        out 64h, al
        mov al, 20h
        out 00h, al
        pop ds
        pop ax
        iret

banner: db "PC98APP DEMO", 0
hint:   db "Z: box  W: write sector 5", 0
wtext:  db "WRITTEN BY THE DEMO", 0
wlen    equ $ - wtext

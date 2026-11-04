; idle.asm: boot sectors for the idle skip's tests (tests/host/test_pc98app.py), written from scratch for them. One source, eight programs,
; chosen with nasm -DMODE=n. Every program keeps its results in low RAM (the BIOS work area starts at 0400h; 0600h-061Fh is free):
;   0600 word  interrupts handled                 0602 word  turns of the main loop completed
;   0604 word  work counter                       0606 word  "OK" once the program runs
;   0608 word  what the wait loops poll           0610 byte  the last key code read from the keyboard
;
;   MODE 1  the games' frame delay, as TH01 and TH04 write it:  counter := 0; do ax := counter while (ax < 1);  the CRT's VSYNC
;           interrupt counts the counter up. Three instructions, no writes: the loop the idle skip exists for
;   MODE 2  the same wait with the read in a subroutine (CALL pushes a return address and RET takes it back: writes that net to zero)
;   MODE 3  no interrupts: waits for the end and then the start of VSYNC by reading the display controller's status port
;   MODE 4  a delay loop that counts in CX (the registers change: never skipped)
;   MODE 5  a loop that increments a counter in memory (the writes change memory: never skipped)
;   MODE 6  the frame delay again, but counted by the interval timer's interrupt (counter 0, 100 Hz) instead of VSYNC
;   MODE 7  a loop that writes a port (the GRCG's tile register 7Eh: every write advances the tile pointer: never skipped)
;   MODE 8  no interrupts: waits for a key at the keyboard's status port and reads it
;   MODE 9  a loop that writes the keyboard's command port: a port that reading is allowed for, so only the write rule stops it
;   MODE 10 the games' start-up state: interrupts on, counter 0 of the timer free-running with a count of 3 (an edge every 24 clocks) but
;           IRQ 0 masked, no VSYNC interrupt armed, and a wait that nothing will ever end: the sources of interrupts are all silent
;
; After every wait the program does a little work (100 increments of the work counter), the way a game does its frame.
        cpu 8086
        bits 16
        org 0

%ifndef MODE
%define MODE 1
%endif

CNT     equ 0600h
DONE    equ 0602h
WORK    equ 0604h
MARK    equ 0606h
WAITCNT equ 0608h
KEYS    equ 0610h

start:  cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov word [MARK], 4B4Fh
%if MODE == 1 || MODE == 2
        mov word [0x28], isr_crtv       ; IRQ 2 (the CRT's VSYNC) is INT 0Ah
        mov [0x2A], cs
        in al, 02h
        and al, 0FBh                    ; unmask IRQ 2 on the master controller
        out 02h, al
        xor al, al
        out 64h, al                     ; arm the first CRT interrupt
        sti
%endif
%if MODE == 6
        mov word [0x20], isr_pit        ; IRQ 0 is INT 08h
        mov [0x22], cs
        mov al, 34h                     ; counter 0, LSB then MSB, mode 2
        out 77h, al
        mov ax, 19968                   ; 1.9968 MHz / 19968 = 100 Hz (the 16 MHz machine's timer clock)
        out 71h, al
        mov al, ah
        out 71h, al
        in al, 02h
        and al, 0FEh                    ; unmask IRQ 0
        out 02h, al
        sti
%endif

%if MODE == 10
        mov al, 36h                     ; counter 0, LSB then MSB, mode 3
        out 77h, al
        mov al, 3
        out 71h, al
        xor al, al
        out 71h, al
        sti
%endif

main:
%if MODE == 1 || MODE == 6 || MODE == 10
        mov word [WAITCNT], 0
.w:     mov ax, [WAITCNT]
        cmp ax, 1
        jc .w
%elif MODE == 2
        mov word [WAITCNT], 0
.w:     call getcnt
        cmp ax, 1
        jc .w
%elif MODE == 3
.a:     in al, 0A0h
        test al, 20h
        jnz .a                          ; VSYNC is on: wait for it to end
.b:     in al, 0A0h
        test al, 20h
        jz .b                           ; and for the next one to begin
%elif MODE == 4
        xor cx, cx
.d:     loop .d
%elif MODE == 5
.v:     inc word [WORK]
        jmp .v
%elif MODE == 7
.o:     out 7Eh, al
        jmp .o
%elif MODE == 9
.p:     out 43h, al
        jmp .p
%elif MODE == 8
.k:     in al, 43h
        test al, 02h
        jz .k                           ; RxRDY
        in al, 41h
        mov [KEYS], al
%endif
        inc word [DONE]
        mov cx, 100
.k2:    inc word [WORK]
        loop .k2
        jmp main

%if MODE == 2
getcnt: mov ax, [WAITCNT]
        ret
%endif

%if MODE == 1 || MODE == 2
isr_crtv:
        push ax
        push ds
        xor ax, ax
        mov ds, ax
        inc word [CNT]
        inc word [WAITCNT]
        xor al, al
        out 64h, al                     ; arm the next one
        mov al, 20h
        out 00h, al                     ; end of interrupt
        pop ds
        pop ax
        iret
%endif
%if MODE == 6
isr_pit:
        push ax
        push bx
        push ds
        xor ax, ax
        mov ds, ax
%ifdef TRACE
        mov bx, [CNT]                   ; the work counter at each interrupt, from 0700h: where the main loop was when it came
        and bx, 3Fh
        shl bx, 1
        mov ax, [WORK]
        mov [0700h + bx], ax
%endif
        inc word [CNT]
        inc word [WAITCNT]
        mov al, 20h
        out 00h, al
        pop ds
        pop bx
        pop ax
        iret
%endif

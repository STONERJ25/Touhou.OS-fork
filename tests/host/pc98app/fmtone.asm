; fmtone.asm: a boot sector that makes the FM board (the PC-9801-86's YM2608) play a steady tone on its SSG channel A, for the tests of the sound path
; (tests/host/test_pc98app.py and tests/case_pc98app.py). Written from the chip's register map (docs/PC98_SOUND.md). The frequency of an SSG tone on the -86
; board is 7,987,200 Hz / (128 x period): period 62 is 1006.5 Hz.
;
;   register 00h/01h  tone period of channel A (low byte, high nibble)      07h  mixer: bit 0 = 0 switches the tone of A on (bits 3-5 = 1: no noise)
;   register 08h      level of channel A (0-15)
; The program then waits in a loop that reads nothing and writes nothing (what a game does between frames), so the idle skip may take it.
        cpu 8086
        bits 16
        org 0

start:  cli
        mov al, 00h
        mov dx, 188h
        out dx, al
        mov al, 62
        mov dx, 18Ah
        out dx, al
        mov al, 01h
        mov dx, 188h
        out dx, al
        mov al, 0
        mov dx, 18Ah
        out dx, al
        mov al, 07h
        mov dx, 188h
        out dx, al
        mov al, 38h
        mov dx, 18Ah
        out dx, al
        mov al, 08h
        mov dx, 188h
        out dx, al
        mov al, 0Fh
        mov dx, 18Ah
        out dx, al
.spin:  jmp .spin

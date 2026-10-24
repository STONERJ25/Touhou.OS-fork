; Sieve of Eratosthenes below 1000: counts the primes (168) and adds them up in 32 bits (76127 = 1295Fh) with ADD/ADC.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286
; expect: ax=00a8 dx=0001 bx=295f si=03e8
        cpu 8086
        bits 16
        org 100h

N       equ 1000

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        mov di, sieve
        mov cx, N
        mov al, 1
        cld
        rep stosb               ; every number starts as a candidate
        mov byte [sieve], 0
        mov byte [sieve+1], 0
        mov si, 2
outer:  mov ax, si
        mul si                  ; ax = si * si
        cmp ax, N
        jae count
        cmp byte [sieve+si], 0
        je next
        mov bx, ax
mark:   mov byte [sieve+bx], 0
        add bx, si
        cmp bx, N
        jb mark
next:   inc si
        jmp outer

count:  xor cx, cx              ; primes found
        xor bx, bx              ; sum, low word
        xor dx, dx              ; sum, high word
        xor si, si
cloop:  cmp byte [sieve+si], 0
        je cnext
        inc cx
        add bx, si
        adc dx, 0
cnext:  inc si
        cmp si, N
        jb cloop
        mov ax, cx
        hlt

sieve:  times N db 0

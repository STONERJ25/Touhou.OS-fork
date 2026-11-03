; The 386 instruction set the way real-mode programs use it: Fibonacci in 32 bits, a 64-bit product and shifts
; (MUL, SHLD, SHRD, SAR), 32-bit DIV and IDIV, MOVZX/MOVSX/CWDE/CDQ, a sieve on a bit array (BTS and BT with bit
; numbers beyond the first doubleword), BSF/BSR, SETcc, scaled-index addressing, REP STOSD/MOVSD, FS/GS loads and
; overrides (an OR into FS memory, as plane blitters do), PUSH imm32 with IMUL r32, imm, near Jcc, PUSHAD/POPAD.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; The expected bytes were computed separately (Python arithmetic), not taken from the emulator.
; models: 386 486
; expect: ax=00ac bx=393d
; expect-mem: 1000:0400 cb 7e 19 06 80 20 2d 24 4e ea 00 0b 00 00 08 d2
; expect-mem: 1000:0410 42 e2 a4 0e 20 2d 24 4e ea 00 00 00 6c 3c 01 00
; expect-mem: 1000:0420 fb 15 00 00 94 c3 fe ff 05 ea ff ff 80 ff ff ff
; expect-mem: 1000:0430 ff 00 00 00 01 80 ff ff 00 90 ff ff ff ff ff ff
; expect-mem: 1000:0440 15 00 17 00 09 00 c7 8a a9 00 33 33 33 33 ef be
; expect-mem: 1000:0450 ad de ef be ad de ef be ad de ef be ad de 33 33
; expect-mem: 1000:0460 33 33 7f 7f 7f 7f 68 03 9d 36 08
        cpu 386
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        cld
        xor eax, eax            ; 1. fib(40) = 102334155
        mov ebx, 1
        mov cx, 40
fib:    lea edx, [eax+ebx]
        mov eax, ebx
        mov ebx, edx
        loop fib
        mov [r_fib], eax
        mov eax, 12345678h      ; 2. 12345678h * 9ABCDEF0h as a 64-bit product
        mov ebx, 9ABCDEF0h
        mul ebx
        mov [r_mul], eax
        mov [r_mul+4], edx
        shld edx, eax, 12       ; the product shifted left by 12 as one 64-bit value
        shl eax, 12
        mov [r_shl], eax
        mov [r_shl+4], edx
        shrd eax, edx, 20       ; then arithmetically right by 20
        sar edx, 20
        mov [r_sar], eax
        mov [r_sar+4], edx
        mov eax, 1000000007     ; 3. 1000000007 / 12345 and -1000000007 / 12345
        xor edx, edx
        mov ecx, 12345
        div ecx
        mov [r_div], eax
        mov [r_div+4], edx
        mov eax, -1000000007
        cdq
        idiv ecx
        mov [r_idiv], eax
        mov [r_idiv+4], edx
        movsx eax, byte [b80]   ; 4. extensions
        mov [r_ext], eax
        movzx eax, byte [bff]
        mov [r_ext+4], eax
        movsx eax, word [w8001]
        mov [r_ext+8], eax
        mov ax, 9000h
        cwde
        mov [r_ext+12], eax
        mov eax, 80000000h
        cdq
        mov [r_ext+16], edx

        mov edi, bitmap           ; 5. sieve below 1024 on a bit array: a set bit marks a composite
        xor eax, eax
        mov ecx, 32             ; 32 doublewords
        rep stosd
        mov ecx, 2
outer:  mov eax, ecx
        mul eax                 ; EAX = i*i
        cmp eax, 1024
        jae near count
        bt [bitmap], ecx          ; i itself composite?
        jc next
mark:   bts [bitmap], eax
        add eax, ecx
        cmp eax, 1024
        jb mark
next:   inc ecx
        jmp outer
count:  xor ax, ax              ; AX = number of primes, BX = their sum modulo 64K
        xor bx, bx
        mov ecx, 2
cloop:  bt [bitmap], ecx
        jc cnext
        inc ax
        add bx, cx
cnext:  inc ecx
        cmp ecx, 1024
        jb cloop
        push ax
        push bx

        mov eax, 00A00000h      ; 6. bit scans
        bsf ecx, eax
        bsr edx, eax
        mov [r_bsx], cx
        mov [r_bsx+2], dx
        mov eax, 5              ; 7. SETcc: 5 < 7 signed (L), not above, ... packed into one byte
        cmp eax, 7
        setl al
        setg bl
        sete cl
        setb dl
        shl bl, 1
        shl cl, 2
        shl dl, 3
        or al, bl
        or al, cl
        or al, dl
        mov [r_set], al
        xor eax, eax            ; 8. the sum of eight doublewords through [esi+ecx*4]
        xor ecx, ecx
        mov esi, array
asum:   add eax, [esi+ecx*4]
        inc ecx
        cmp ecx, 8
        jb asum
        mov [r_sum], eax
        mov ebx, qwords         ; [ebx+ecx*8+4]: the high half of the third quadword
        mov ecx, 2
        mov eax, [ebx+ecx*8+4]
        mov [r_q], eax
        mov eax, 0DEADBEEFh     ; 9. REP STOSD four times, then REP MOVSD two of them away
        mov edi, fill
        mov cx, 4
        rep stosd
        mov si, fill
        mov di, r_copy
        mov cx, 2
        rep movsd
        mov ax, cs              ; 10. FS and GS: a load from each, an OR into FS memory
        mov fs, ax
        mov gs, ax
        mov eax, [fs:fsv]
        add eax, [gs:gsv]
        mov [r_fsgs], eax
        mov eax, 1F1F1F1Fh
        or [fs:orv], eax
        push dword 12345678h    ; 11. PUSH imm32, IMUL r32, r/m32, imm
        pop ecx
        imul ecx, ecx, 3
        mov [r_imul], ecx
        mov eax, 1              ; 12. PUSHAD/POPAD around clobbered registers, a near Jcc over a long stretch
        mov ebp, 2
        pushad
        mov eax, -1
        mov ebp, -1
        popad
        add eax, ebp
        cmp eax, 3
        jne near bad
        mov byte [r_ok], 8
        times 140 nop           ; out of a short jump's reach
bad:    pop bx
        pop ax
        hlt

b80:    db 80h
bff:    db 0FFh
w8001:  dw 8001h
array:  dd 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000
qwords: dq 1111111111111111h, 2222222222222222h, 3333333344444444h
fsv:    dd 11111111h
gsv:    dd 22222222h

        times 300h - ($ - $$) db 0
r_fib:  dd 0                    ; 1000:0400
r_mul:  dd 0, 0                 ; 1000:0404
r_shl:  dd 0, 0                 ; 1000:040C
r_sar:  dd 0, 0                 ; 1000:0414
r_div:  dd 0, 0                 ; 1000:041C
r_idiv: dd 0, 0                 ; 1000:0424
r_ext:  dd 0, 0, 0, 0, 0        ; 1000:042C
r_bsx:  dw 0, 0                 ; 1000:0440
r_set:  db 0                    ; 1000:0444
        db 0
r_sum:  dd 0                    ; 1000:0446
r_q:    dd 0                    ; 1000:044A
fill:   dd 0, 0                 ; 1000:044E (the first two of the four STOSD doublewords)
r_copy: dd 0, 0                 ; 1000:0456 (overwrites the last two with the copy)
r_fsgs: dd 0                    ; 1000:045E
orv:    dd 60606060h            ; 1000:0462
r_imul: dd 0                    ; 1000:0466
r_ok:   db 0                    ; 1000:046A
bitmap: times 128 db 0

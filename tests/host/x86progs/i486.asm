; The 486 additions: BSWAP, XADD, CMPXCHG (equal and not equal, with LOCK), the AC flag sticking in EFLAGS, and INVD,
; WBINVD and INVLPG running as no-ops (there is no cache to flush).
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 486
; expect-mem: 1000:0400 78 56 34 12 2a 00 00 00 0a 00 00 00 63 00 00 00 01 00 63 00 00 00 01
        cpu 486
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov es, ax
        mov eax, 78563412h
        bswap eax
        mov [r_bswap], eax      ; 12345678h
        mov eax, 10
        mov ebx, 32
        xadd eax, ebx           ; EAX = 42, EBX = 10
        mov [r_xadd], eax
        mov [r_xadd+4], ebx
        mov dword [lockv], 5
        mov eax, 5
        mov ecx, 99
        ; lock cmpxchg [lockv], ecx (F0 66 0F B1 /r: NASM files CMPXCHG under the Pentium, so the bytes are spelt out)
        db 0F0h, 66h, 0Fh, 0B1h, 0Eh
        dw lockv                ; equal: [lockv] = 99, ZF = 1
        setz byte [r_z1]
        mov eax, 7
        db 0F0h, 66h, 0Fh, 0B1h, 0Eh
        dw lockv                ; not equal: EAX = [lockv] = 99, ZF = 0
        setz byte [r_z2]
        mov [r_eax2], eax
        pushfd                  ; AC (EFLAGS bit 18) sticks on a 486
        pop eax
        or eax, 40000h
        push eax
        popfd
        pushfd
        pop eax
        shr eax, 18
        and al, 1
        mov [r_ac], al
        invd
        wbinvd
        invlpg [lockv]
        hlt

        times 300h - ($ - $$) db 0
r_bswap: dd 0                   ; 1000:0400
r_xadd: dd 0, 0                 ; 1000:0404
lockv:  dd 0                    ; 1000:040C
r_z1:   db 0                    ; 1000:0410
r_z2:   db 0
r_eax2: dd 0                    ; 1000:0412
r_ac:   db 0                    ; 1000:0416

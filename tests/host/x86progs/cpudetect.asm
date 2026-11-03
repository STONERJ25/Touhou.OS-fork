; CPU identification the way real-mode programs of the 386 era do it (the game executables' runtime runs a probe of this
; kind at start-up): FLAGS bits 12-15 stuck at 1 means 8086 or V30 (AAD 16 then tells Intel from NEC), stuck at 0 means a
; 286 in real mode; bits 12-14 writable means a 386 or later; an AC flag (EFLAGS bit 18) that can be toggled means a 486;
; an ID flag (bit 21) that can be toggled would mean CPUID exists, which neither modelled chip has.
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30 286 386 486
; expect[8086]: ax=0000 dx=0000
; expect[v30]: ax=0001 dx=0000
; expect[286]: ax=0002 dx=0000
; expect[386]: ax=0003 dx=0000 si=0000
; expect[486]: ax=0004 dx=0000 si=0001
        cpu 386
        bits 16
        org 100h

start:  xor dx, dx
        xor si, si
        pushf                   ; 1. clear FLAGS bits 12-15 and look at them again
        pop ax
        and ax, 0FFFh
        push ax
        popf
        pushf
        pop ax
        and ax, 0F000h
        cmp ax, 0F000h
        jne not86
        mov ax, 0101h           ; 8086 or V30: AAD 16 gives 11h on Intel, 0Bh on NEC
        db 0D5h, 10h
        cmp al, 11h
        je is8086
        mov ax, 1
        jmp done
is8086: xor ax, ax
        jmp done

not86:  mov ax, 7000h           ; 2. try to set IOPL and NT: a 286 in real mode keeps them at 0
        push ax
        popf
        pushf
        pop ax
        test ax, 7000h
        jnz is386
        mov ax, 2
        jmp done

is386:  pushfd                  ; 3. toggle AC; the 386 does not have it
        pop eax
        mov ecx, eax
        xor eax, 40000h
        push eax
        popfd
        pushfd
        pop eax
        push ecx                ; put EFLAGS back
        popfd
        xor eax, ecx
        test eax, 40000h
        jnz is486
        mov ax, 3
        jmp done
is486:  mov si, 1               ; AC toggled
        pushfd                  ; 4. toggle ID: only a chip with CPUID can
        pop eax
        mov ecx, eax
        xor eax, 200000h
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        shr eax, 21
        and ax, 1
        mov dx, ax
        mov ax, 4
done:   hlt

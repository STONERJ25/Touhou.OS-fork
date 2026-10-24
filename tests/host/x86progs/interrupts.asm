; Interrupts: a divide error (the 8086 and V30 return to the instruction after the DIV), INT3, INT n with a handler that
; returns a value and sets CF through the FLAGS image on the stack, INTO taken and not taken, and single-stepping with
; TF (the instruction that clears TF is still trapped: 8 traps).
; Written for the TouhouOS x86emu tests (tests/host/test_x86progs.py); loaded at 1000:0100, runs until HLT.
; models: 8086 v30
; expect: sp=fffe
; expect-mem: 1000:0400 01 01 01 00 2a 00 ff ff 08 00
        cpu 8086
        bits 16
        org 100h

start:  mov ax, cs
        mov ds, ax
        mov ss, ax
        mov sp, 0FFFEh
        xor ax, ax
        mov es, ax
        mov word [es:0*4], isr_div
        mov [es:0*4+2], cs
        mov word [es:1*4], isr_step
        mov [es:1*4+2], cs
        mov word [es:3*4], isr_bp
        mov [es:3*4+2], cs
        mov word [es:4*4], isr_ovf
        mov [es:4*4+2], cs
        mov word [es:60h*4], isr_svc
        mov [es:60h*4+2], cs
        xor bx, bx              ; divide by zero
        mov ax, 1234h
        div bl
        int3
        mov ax, 21
        int 60h                 ; ax = 42 and CF set by the handler
        mov [r_svc], ax
        sbb cx, cx
        mov [r_cf], cx
        mov al, 7Fh             ; signed overflow: INTO is taken
        add al, 1
        into
        mov al, 1               ; no overflow: INTO falls through
        add al, 1
        into
        pushf                   ; single step
        pop ax
        or ax, 100h
        push ax
        popf
        nop
        nop
        nop
        pushf
        pop ax
        and ax, 0FEFFh
        push ax
        popf
        hlt

isr_div:  inc byte [r_div]
          iret
isr_step: inc word [r_step]
          iret
isr_bp:   inc byte [r_bp]
          iret
isr_ovf:  inc byte [r_ovf]
          iret
isr_svc:  shl ax, 1
          push bp
          mov bp, sp
          or word [bp+6], 1     ; [bp+2] IP, [bp+4] CS, [bp+6] FLAGS
          pop bp
          iret

        times 300h - ($ - $$) db 0
r_div:  db 0                    ; 1000:0400
r_bp:   db 0
r_ovf:  db 0
        db 0
r_svc:  dw 0                    ; 1000:0404
r_cf:   dw 0                    ; 1000:0406
r_step: dw 0                    ; 1000:0408

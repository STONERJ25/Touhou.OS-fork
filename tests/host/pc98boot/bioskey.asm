; bioskey.asm: a boot sector that exercises the HLE BIOS's keyboard: INT 09h (the keyboard interrupt fills the buffer
; at 0000:0502) and INT 18h AH=00h, 01h, 02h, 04h, 05h (user/lib/pc98/src/pc98bios.c, docs/PC98_BIOS.md). Written for
; tests/host/test_pc98bios.py, which types with pc98run --keys "aA^c\r1!z" and twenty "x" after that.
;
; Results at 0000:0600: for each of the first six keys, the word AH=00h returned, the shift state (AH=02h) and key
; group 0Eh (AH=04h) right after it; at 620 the key count after a stray INT 09h (0); then BH of a sense (AH=01h) and of
; a read without waiting (AH=05h) with the buffer empty; then, after "z" and a pause long enough for the twenty "x" to
; arrive, how many keys AH=05h could take out of the buffer (it holds 16) and the first of them. The program ends
; waiting for a key that never comes.
;
;   nasm -f bin -o bioskey.bin bioskey.asm

        bits 16
        org 0

start:
        sti                             ; the keyboard interrupt must get through while we wait
        mov di, 0x600
        mov cx, 6
.key:   mov ah, 0x00                    ; read a key, waiting
        int 0x18
        mov [di], ax
        mov ah, 0x02                    ; shift state
        int 0x18
        mov [di+2], al
        mov ax, 0x040e                  ; key group 0Eh: SHIFT CAPS KANA GRPH CTRL
        int 0x18
        mov [di+3], ah
        add di, 4
        loop .key                       ; 600 .. 617

        int 0x09                        ; a stray keyboard interrupt: the 8251 still holds the last make code, already
        mov al, [0x528]                 ; taken, so no key may appear (the break comes 30 ms later)
        mov [0x620], al                 ; 620
        mov ah, 0x01                    ; sense with nothing there
        mov bh, 0x55
        int 0x18
        mov [di], bh                    ; 618
        mov ah, 0x05                    ; read without waiting, nothing there
        mov bh, 0x55
        int 0x18
        mov [di+1], bh                  ; 619
        add di, 2

        mov ah, 0x00                    ; "z"
        int 0x18
        mov [di], ax                    ; 61A
        mov dx, 16                      ; about 1.8 s at 10 MHz with interrupts on: twenty "x" arrive meanwhile
.wait:  xor cx, cx
.spin:  loop .spin
        dec dx
        jnz .wait
        xor si, si                      ; count what is in the buffer
.drain: mov ah, 0x05
        int 0x18
        cmp bh, 1
        jne .done
        or si, si
        jnz .next
        mov [di+4], ax                  ; 61E: the first of them
.next:  inc si
        jmp .drain
.done:  mov [di+2], si                  ; 61C
        mov ah, 0x00                    ; wait for a key that never comes (the test stops here)
        int 0x18
        cli
        hlt

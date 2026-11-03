; Calling firmware code: the UEFI runtime services use the Microsoft x64 convention (arguments in rcx, rdx, r8, r9, 32 bytes of shadow space, the stack
; 16-byte aligned at the call), the kernel the System V one, and the kernel is compiled without SSE registers, which the compiler's ms_abi support wants.
;
; uint64_t efi_call4(uint64_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
;     System V in: rdi = fn, rsi = a, rdx = b, rcx = c, r8 = d
bits 64
default rel

section .text

global efi_call4

efi_call4:
    push rbp
    mov rbp, rsp
    and rsp, -16
    sub rsp, 32                     ; the shadow space
    mov rax, rdi
    mov r9, r8                      ; d
    mov r8, rcx                     ; c
    mov rcx, rsi                    ; a   (b stays in rdx)
    call rax
    mov rsp, rbp
    pop rbp
    ret

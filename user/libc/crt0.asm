; Program entry: the kernel leaves argc at [rsp], then argv[], NULL, envp[], NULL, auxv.
bits 64
default rel

section .text
global _start
extern __libc_start

_start:
    xor ebp, ebp
    mov rdi, rsp
    and rsp, -16
    call __libc_start
    ud2

; Signal handlers "return" here; it issues SYS_sigreturn (54).
global __restore_rt
__restore_rt:
    mov eax, 54
    syscall
    ud2

; Kernel access to user memory. A page fault with rip inside [__uaccess_start, __uaccess_end) that demand paging cannot
; resolve is redirected to __uaccess_fault, which returns the number of bytes not processed (rcx still holds it).
bits 64
default rel

section .text

global __uaccess_start
global __uaccess_end
global __uaccess_fault
global __copy_user
global __clear_user

__uaccess_start:

; size_t __copy_user(void *dst, const void *src, size_t n)  -> bytes NOT copied
__copy_user:
    mov rcx, rdx
    cld
    rep movsb
    mov rax, rcx
    ret

; size_t __clear_user(void *dst, size_t n)  -> bytes NOT cleared
__clear_user:
    mov rcx, rsi
    xor eax, eax
    cld
    rep stosb
    mov rax, rcx
    ret

__uaccess_end:

__uaccess_fault:
    mov rax, rcx
    ret

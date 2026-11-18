; Kernel access to user memory. A page fault with rip inside [__uaccess_start, __uaccess_end) that demand paging cannot
; resolve is redirected to __uaccess_fault, which returns the number of bytes not processed (rcx still holds it).
;
; With SMAP on (CR4.SMAP, kernel/arch/x86_64/cpu.c) a supervisor-mode data access to a user page faults unless EFLAGS.AC is 1 (Intel SDM vol. 3A, 4.6.1), so
; the two routines below are the only places in the kernel that touch user memory directly, and each opens the window with STAC just before the string
; instruction and closes it with CLAC right after it. Everything else reaches a user buffer through them (copy_from_user, copy_to_user, clear_user,
; strnlen_user, strncpy_from_user in kernel/sys/uaccess.c) or through the direct map (uvm_write, the futex words, the exec stack).
;   * the window is open for one `rep movsb` or `rep stosb` and nothing else runs inside it: no call, no sleep, no interrupt handler (an interrupt that arrives
;     inside it is entered with AC cleared by isr.asm and returns with AC as it was);
;   * a fault that cannot be resolved ends in __uaccess_fault, which must close the window too: the instruction pointer was moved there, AC is still 1;
;   * STAC and CLAC are #UD on a CPU without SMAP, so they run only when g_smap_enabled is set (cpu_init: the CPU has SMAP and "smap=off" is not given).
bits 64
default rel

extern g_smap_enabled

section .text

global __uaccess_start
global __uaccess_end
global __uaccess_fault
global __copy_user
global __clear_user

%macro WINDOW_OPEN 0
    cmp byte [g_smap_enabled], 0
    je %%none
    stac
%%none:
%endmacro

%macro WINDOW_CLOSE 0
    cmp byte [g_smap_enabled], 0
    je %%none
    clac
%%none:
%endmacro

__uaccess_start:

; size_t __copy_user(void *dst, const void *src, size_t n)  -> bytes NOT copied
__copy_user:
    mov rcx, rdx
    cld
    WINDOW_OPEN
    rep movsb
    WINDOW_CLOSE
    mov rax, rcx
    ret

; size_t __clear_user(void *dst, size_t n)  -> bytes NOT cleared
__clear_user:
    mov rcx, rsi
    xor eax, eax
    cld
    WINDOW_OPEN
    rep stosb
    WINDOW_CLOSE
    mov rax, rcx
    ret

__uaccess_end:

; Reached by a redirected fault (process_pagefault): the window is still open.
__uaccess_fault:
    WINDOW_CLOSE
    mov rax, rcx
    ret

; Reading kernel memory that may not be mapped. A page fault with rip inside [__kprobe_start, __kprobe_end) is steered to
; __kprobe_fault (trap.c), so a damaged frame pointer chain or a bad stack pointer cannot turn a panic report into a second fault.
bits 64
default rel

section .text

global kprobe_read64
global __kprobe_start
global __kprobe_end
global __kprobe_fault

; int kprobe_read64(const u64 *p, u64 *out): 0, or -1 if p cannot be read
kprobe_read64:
__kprobe_start:
    mov rax, [rdi]
__kprobe_end:
    mov [rsi], rax
    xor eax, eax
    ret
__kprobe_fault:
    mov eax, -1
    ret

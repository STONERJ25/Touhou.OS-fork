; Kernel entry point. The loader jumps here with BootInfo* in rdi, interrupts off,
; running on the loader's page tables (HHDM + kernel image mapped), SysV ABI.
bits 64
default rel

section .text

global _start
extern kmain

_start:
    cli
    cld
    lea rsp, [boot_stack_top]
    xor ebp, ebp
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .bss
align 4096
global boot_stack_bottom
global boot_stack_top
boot_stack_bottom:
    resb 32768
boot_stack_top:
